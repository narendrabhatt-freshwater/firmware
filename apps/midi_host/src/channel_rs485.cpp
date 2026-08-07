#include "channel_rs485.hpp"

#include "host_io/rs485/bus.hpp"

#include <array>
#include <atomic>
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

double ClampNoteHz(double hz)
{
  if (hz <= 0.0) {
    return 0.0;
  }
  if (hz < 20.0) {
    return 20.0;
  }
  if (hz > 19999.0) {
    return 19999.0;
  }
  return hz;
}

bool HzEqual(double a, double b)
{
  if (a <= 0.0 && b <= 0.0) {
    return true;
  }
  return a == b;
}

} // namespace

struct ChannelRs485Out::Impl
{
  host_io::rs485::Bus bus;

  std::mutex mu_;
  std::condition_variable cv_;
  std::array<double, kVoiceCount> desired_hz_{};
  std::array<double, kVoiceCount> sent_hz_{};
  std::atomic<bool> run_{false};
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
                ? ClampNoteHz(slots[i].freq_hz)
                : 0.0;
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
      if (!HzEqual(desired_hz_[i], sent_hz_[i])) {
        return true;
      }
    }
    return false;
  }

  bool TakeDirtySlot(uint8_t &slot_out, double &hz_out)
  {
    for (uint8_t i = 0; i < kVoiceCount; ++i) {
      if (desired_hz_[i] <= 0.0 && sent_hz_[i] > 0.0) {
        slot_out = i;
        hz_out = 0.0;
        return true;
      }
    }
    for (uint8_t i = 0; i < kVoiceCount; ++i) {
      if (!HzEqual(desired_hz_[i], sent_hz_[i])) {
        slot_out = i;
        hz_out = desired_hz_[i];
        return true;
      }
    }
    return false;
  }

  void MarkSent(uint8_t slot, double hz) { sent_hz_[slot] = hz; }

  void TripHalt(const char *why)
  {
    bool expected = false;
    if (!halted_.compare_exchange_strong(expected, true)) {
      return;
    }
    bus.MarkBusFault();
    {
      std::lock_guard<std::mutex> lock(mu_);
      desired_hz_.fill(0.0);
      sent_hz_.fill(0.0);
    }
    cv_.notify_all();

    std::fprintf(stderr, "\n*** RS485 fault: %s\n", why);
    std::fprintf(stderr, "*** sending silence — note output STOPPED\n");
    std::fprintf(stderr,
                 "*** quit (Enter) and restart midi_host after fixing the bus\n\n");

    if (!bus.SoftRecover()) {
      bus.ForceClearBus();
    }
  }

  void WorkerMain()
  {
    while (true) {
      uint8_t slot = 0;
      double hz = 0.0;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] {
          if (!run_.load()) {
            return true;
          }
          if (halted_.load()) {
            return false;
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
        if (HzEqual(hz, sent_hz_[slot])) {
          continue;
        }
      }

      auto print_ok = [&](double ack_hz) {
        if (ack_hz <= 0.0) {
          std::printf("ok     slot=%-2u  n%X off\n",
                      static_cast<unsigned>(slot),
                      static_cast<unsigned>(slot));
        } else {
          std::printf("ok     slot=%-2u  n%X %.2f Hz\n",
                      static_cast<unsigned>(slot),
                      static_cast<unsigned>(slot), ack_hz);
        }
      };

      protocol::Result r = bus.Channel().SetNote(slot, hz);
      if (r.ok()) {
        {
          std::lock_guard<std::mutex> lock(mu_);
          MarkSent(slot, hz);
        }
        print_ok(hz);
        continue;
      }

      if (r.status == protocol::Status::Err) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "card err:%s on n%X %.2f", r.err_code,
                      static_cast<unsigned>(slot), hz);
        TripHalt(msg);
        continue;
      }

      if (r.status == protocol::Status::IoError) {
        TripHalt("serial I/O error (no reliable ACK path)");
        continue;
      }

      char msg[192];
      std::snprintf(msg, sizeof(msg), "no [C]ok for n%X %.2f — RX: %s",
                    static_cast<unsigned>(slot), hz,
                    r.raw[0] != '\0' ? r.raw : "(empty)");
      TripHalt(msg);
    }
  }

  void StartWorker()
  {
    desired_hz_.fill(0.0);
    sent_hz_.fill(0.0);
    halted_ = false;
    run_ = true;
    worker_ = std::thread([this] { WorkerMain(); });
  }

  void StopWorker()
  {
    {
      std::lock_guard<std::mutex> lock(mu_);
      desired_hz_.fill(0.0);
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
                           host_io::rs485::EffectEcho effect_echo)
{
  Close();
  if (atten_db > 127u) {
    throw std::runtime_error("gain atten_db must be 0..127");
  }
  atten_db_ = atten_db;

  host_io::rs485::BusOptions opts;
  opts.baud = baud;
  opts.atten_db = atten_db;
  opts.reply_timeout_ms = 400;
  opts.retries = 0;
  opts.idle_gap_ms = 0;
  opts.post_ack_settle_ms = 0;
  opts.post_tx_settle_ms = 0;
  opts.late_ack_grace_ms = 0;
  opts.rx_idle_ms = 0;
  opts.rx_idle_max_ms = 0;
  opts.effect_echo = effect_echo;
  opts.allow_missing_effect = true;

  const protocol::Result r = impl_->bus.Open(serial_path, opts);
  if (!r.ok()) {
    std::string msg = "RS485 session open failed";
    if (r.raw[0] != '\0') {
      msg += ": ";
      msg += r.raw;
    } else if (r.status == protocol::Status::Timeout) {
      msg += " (timeout — check wiring / Channel Card power)";
    } else if (r.status == protocol::Status::Err) {
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
  impl_->bus.Close();
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
  return impl_ && impl_->bus.EchoDisabled();
}

bool ChannelRs485Out::BusFault() const
{
  return impl_ && (impl_->halted_.load() || impl_->bus.BusFault());
}

} // namespace midi_host
