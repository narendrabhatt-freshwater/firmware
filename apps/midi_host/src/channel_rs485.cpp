#include "channel_rs485.hpp"

#include "rs485/session.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace midi_host
{
namespace
{

uint16_t HzToU16(double hz)
{
  if (hz <= 0.0) {
    return 0;
  }
  long v = std::lround(hz);
  if (v < 20) {
    return 20;
  }
  if (v > 19999) {
    return 19999;
  }
  return static_cast<uint16_t>(v);
}

} // namespace

struct ChannelRs485Out::Impl
{
  rs485::Session session;

  std::mutex mu_;
  std::condition_variable cv_;
  /**
   * Absolute desired Hz per slot (0 = off). Latest VoiceBank snapshot wins —
   * coalesces On/Off storms so a late Off cannot be dropped behind a full queue.
   */
  std::array<uint16_t, kVoiceCount> desired_hz_{};
  /** Last Hz we believe the card has (only advanced on Status::Ok). */
  std::array<uint16_t, kVoiceCount> sent_hz_{};
  std::atomic<bool> run_{false};
  std::thread worker_;
  uint32_t fault_count_ = 0;

  void PublishBank(const VoiceBank &bank)
  {
    {
      std::lock_guard<std::mutex> lock(mu_);
      const auto &slots = bank.Slots();
      for (uint8_t i = 0; i < kVoiceCount; ++i) {
        const uint16_t hz =
            (slots[i].active && slots[i].freq_hz > 0.0)
                ? HzToU16(slots[i].freq_hz)
                : static_cast<uint16_t>(0);
        desired_hz_[i] = hz;
      }
    }
    cv_.notify_one();
  }

  /** Pick next slot to sync: Offs first (clear hangs), then Ons. */
  bool TakeDirtySlot(uint8_t &slot_out, uint16_t &hz_out)
  {
    for (uint8_t i = 0; i < kVoiceCount; ++i) {
      if (desired_hz_[i] == 0 && sent_hz_[i] != 0) {
        slot_out = i;
        hz_out = 0;
        return true;
      }
    }
    for (uint8_t i = 0; i < kVoiceCount; ++i) {
      if (desired_hz_[i] != sent_hz_[i]) {
        slot_out = i;
        hz_out = desired_hz_[i];
        return true;
      }
    }
    return false;
  }

  void MarkSent(uint8_t slot, uint16_t hz)
  {
    sent_hz_[slot] = hz;
  }

  void ClearCardBelief()
  {
    sent_hz_.fill(0);
    /* desired stays as host VoiceBank wants — next loop will re-On held keys. */
  }

  void WorkerMain()
  {
    while (true) {
      uint8_t slot = 0;
      uint16_t hz = 0;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] {
          if (!run_.load()) {
            return true;
          }
          return TakeDirtySlot(slot, hz);
        });
        if (!run_.load()) {
          if (!TakeDirtySlot(slot, hz)) {
            return;
          }
        } else if (!TakeDirtySlot(slot, hz)) {
          continue;
        }
        /* Always TX the latest desired for this slot (may have moved since
         * the wait predicate peeked an older value). */
        hz = desired_hz_[slot];
        if (hz == sent_hz_[slot]) {
          continue;
        }
      }

      if (session.BusFault()) {
        if (session.SoftRecover()) {
          std::lock_guard<std::mutex> lock(mu_);
          ClearCardBelief();
          ++fault_count_;
          std::fprintf(stderr,
                       "(rs485: recovered after fault — silence; re-syncing)\n");
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          continue;
        }
      }

      const rs485::ExchangeResult r = session.SetNote(slot, hz);
      if (r.ok()) {
        std::lock_guard<std::mutex> lock(mu_);
        /* Record what the card actually applied. If desired moved during
         * the round-trip (e.g. key released while On was in flight),
         * sent != desired and the next loop sends Off — never leave the
         * card sounding while sent still looks "clean". */
        MarkSent(slot, hz);
        continue;
      }

      if (r.status == rs485::Status::Err) {
        std::fprintf(stderr, "(rs485: err:%s on n%X %u)\n", r.err_code,
                     static_cast<unsigned>(slot), static_cast<unsigned>(hz));
        continue;
      }

      session.MarkBusFault();
      if (session.SoftRecover()) {
        std::lock_guard<std::mutex> lock(mu_);
        ClearCardBelief();
        ++fault_count_;
        std::fprintf(stderr,
                     "(rs485: timeout — silence + re-sync from host bank)\n");
      } else {
        std::fprintf(stderr,
                     "(rs485: timeout and silence failed — will retry)\n");
      }
    }
  }

  void StartWorker()
  {
    desired_hz_.fill(0);
    sent_hz_.fill(0);
    run_ = true;
    worker_ = std::thread([this] { WorkerMain(); });
  }

  void StopWorker()
  {
    {
      std::lock_guard<std::mutex> lock(mu_);
      desired_hz_.fill(0);
      run_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
  }
};

ChannelRs485Out::ChannelRs485Out() : impl_(std::make_unique<Impl>()) {}

ChannelRs485Out::~ChannelRs485Out() { Close(); }

void ChannelRs485Out::Open(const std::string &serial_path,
                           uint32_t baud,
                           uint32_t atten_db,
                           rs485::EffectEcho effect_echo)
{
  Close();
  if (atten_db > 127u) {
    throw std::runtime_error("gain atten_db must be 0..127");
  }
  atten_db_ = atten_db;

  rs485::SessionOptions opts;
  opts.baud = baud;
  opts.atten_db = atten_db;
  opts.reply_timeout_ms = 400;
  opts.retries = 1;
  opts.effect_echo = effect_echo;
  opts.allow_missing_effect = true;

  const rs485::ExchangeResult r = impl_->session.Open(serial_path, opts);
  if (!r.ok()) {
    std::string msg = "RS485 session open failed";
    if (r.raw[0] != '\0') {
      msg += ": ";
      msg += r.raw;
    } else if (r.status == rs485::Status::Timeout) {
      msg += " (timeout — check wiring / Channel Card power)";
    } else if (r.status == rs485::Status::Err) {
      msg += " (err:";
      msg += r.err_code;
      msg += ")";
    }
    throw std::runtime_error(msg);
  }

  path_ = serial_path;
  impl_->StartWorker();
}

void ChannelRs485Out::Close()
{
  if (!impl_) {
    return;
  }
  if (impl_->run_.load()) {
    impl_->StopWorker();
  }
  impl_->session.Close();
  path_.clear();
}

void ChannelRs485Out::ApplyBankEvent(const BankEvent &event,
                                     const VoiceBank &bank)
{
  if (!impl_ || !impl_->run_.load()) {
    return;
  }
  if (event.kind == BankEventKind::Steal) {
    /* Bank already holds the replacement; full snapshot below covers it. */
  }
  impl_->PublishBank(bank);
}

bool ChannelRs485Out::EffectEchoDisabled() const
{
  return impl_ && impl_->session.EchoDisabled();
}

bool ChannelRs485Out::BusFault() const
{
  return impl_ && impl_->session.BusFault();
}

} // namespace midi_host
