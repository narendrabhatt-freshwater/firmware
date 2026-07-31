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
  std::array<uint16_t, kVoiceCount> desired_hz_{};
  /** Updated only on [C]ok. */
  std::array<uint16_t, kVoiceCount> sent_hz_{};
  std::atomic<bool> run_{false};
  /** Missing ACK / I/O — silence once, stop all further note TX. */
  std::atomic<bool> halted_{false};
  std::thread worker_;

  void PublishBank(const VoiceBank &bank)
  {
    if (halted_.load()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      const auto &slots = bank.Slots();
      for (uint8_t i = 0; i < kVoiceCount; ++i) {
        desired_hz_[i] =
            (slots[i].active && slots[i].freq_hz > 0.0)
                ? HzToU16(slots[i].freq_hz)
                : static_cast<uint16_t>(0);
      }
    }
    cv_.notify_one();
  }

  bool WorkPending() const
  {
    if (halted_.load()) {
      return false;
    }
    for (uint8_t i = 0; i < kVoiceCount; ++i) {
      if (desired_hz_[i] != sent_hz_[i]) {
        return true;
      }
    }
    return false;
  }

  /** Offs first, then Ons (lowest index). */
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

  void MarkSent(uint8_t slot, uint16_t hz) { sent_hz_[slot] = hz; }

  /**
   * Bus dead: one silence attempt, no more notes until process restart.
   * send → wait → ACK; no ACK → stop. Do not keep TX'ing into a dead bus.
   */
  void TripHalt(const char *why)
  {
    bool expected = false;
    if (!halted_.compare_exchange_strong(expected, true)) {
      return;
    }
    session.MarkBusFault();
    {
      std::lock_guard<std::mutex> lock(mu_);
      desired_hz_.fill(0);
      sent_hz_.fill(0);
    }
    cv_.notify_all();

    std::fprintf(stderr, "\n*** RS485 fault: %s\n", why);
    std::fprintf(stderr, "*** sending silence — note output STOPPED\n");
    std::fprintf(stderr, "*** quit (Enter) and restart midi_host after fixing the bus\n\n");

    if (!session.SoftRecover()) {
      session.ForceClearBus();
    }
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
          if (halted_.load()) {
            return false; /* sleep until Close sets run_=false */
          }
          return WorkPending();
        });
        if (!run_.load()) {
          return;
        }
        if (halted_.load()) {
          continue;
        }
        if (!TakeDirtySlot(slot, hz)) {
          continue;
        }
        hz = desired_hz_[slot];
        if (hz == sent_hz_[slot]) {
          continue;
        }
      }

      auto print_ok = [&](uint16_t ack_hz) {
        if (ack_hz == 0) {
          std::printf("ok     slot=%-2u  n%X off\n",
                      static_cast<unsigned>(slot),
                      static_cast<unsigned>(slot));
        } else {
          std::printf("ok     slot=%-2u  n%X %u Hz\n",
                      static_cast<unsigned>(slot),
                      static_cast<unsigned>(slot),
                      static_cast<unsigned>(ack_hz));
        }
      };

      rs485::ExchangeResult r = session.SetNote(slot, hz);
      if (r.ok()) {
        {
          std::lock_guard<std::mutex> lock(mu_);
          MarkSent(slot, hz);
        }
        print_ok(hz);
        continue;
      }

      if (r.status == rs485::Status::Err) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "card err:%s on n%X %u", r.err_code,
                      static_cast<unsigned>(slot), static_cast<unsigned>(hz));
        TripHalt(msg);
        continue;
      }

      if (r.status == rs485::Status::IoError) {
        TripHalt("serial I/O error (no reliable ACK path)");
        continue;
      }

      /*
       * Empty RX after a long streak is usually USB/DE turnaround, not a
       * dead card. One cool-off + same-command retry before fail-stop.
       */
      std::fprintf(stderr,
                   "(rs485: no cok n%X %u RX:%s — cool-off retry)\n",
                   static_cast<unsigned>(slot), static_cast<unsigned>(hz),
                   r.raw[0] != '\0' ? r.raw : "(empty)");
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      {
        std::lock_guard<std::mutex> lock(mu_);
        hz = desired_hz_[slot]; /* may have changed (Off) during wait */
      }
      r = session.SetNote(slot, hz);
      if (r.ok()) {
        {
          std::lock_guard<std::mutex> lock(mu_);
          MarkSent(slot, hz);
        }
        print_ok(hz);
        continue;
      }

      char msg[192];
      std::snprintf(msg, sizeof(msg),
                    "no [C]ok for n%X %u after retry — RX: %s",
                    static_cast<unsigned>(slot), static_cast<unsigned>(hz),
                    r.raw[0] != '\0' ? r.raw : "(empty)");
      TripHalt(msg);
    }
  }

  void StartWorker()
  {
    desired_hz_.fill(0);
    sent_hz_.fill(0);
    halted_ = false;
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
  opts.reply_timeout_ms = 500;
  opts.retries = 2;
  opts.idle_gap_ms = 2;
  opts.late_ack_grace_ms = 100;
  opts.rx_idle_ms = 10;
  opts.rx_idle_max_ms = 120;
  opts.post_ack_settle_ms = 25;
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

void ChannelRs485Out::ApplyBankEvent(const BankEvent & /*event*/,
                                     const VoiceBank &bank)
{
  if (!impl_ || !impl_->run_.load() || impl_->halted_.load()) {
    return;
  }
  impl_->PublishBank(bank);
}

bool ChannelRs485Out::EffectEchoDisabled() const
{
  return impl_ && impl_->session.EchoDisabled();
}

bool ChannelRs485Out::BusFault() const
{
  return impl_ && (impl_->halted_.load() || impl_->session.BusFault());
}

} // namespace midi_host
