#include "channel_rs485.hpp"

#include "cardlink/rs485/bus.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace midi_host
{
namespace
{

constexpr unsigned kVqVoices = 8;
/* 10 ms while notes are active. Do not go faster — UART TX steals USB SOF. */
constexpr auto kVqPollInterval = std::chrono::milliseconds(10);
constexpr int kVqMissLimit = 16;

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

/** Parse `ok:vq <mask> <best> s0..s7`. */
bool ParseVq(const char *raw, uint8_t &mask_out, uint8_t &best_out,
             std::array<uint8_t, kVqVoices> &slots_out)
{
  cardproto::VoiceQuery q;
  if (!cardproto::ParseVoiceQuery(raw, q)) {
    return false;
  }
  mask_out = q.mask;
  best_out = q.best;
  slots_out = q.free_slots;
  return true;
}

} // namespace

struct ChannelRs485Out::Impl
{
  cardlink::rs485::Bus bus;

  std::mutex mu_;
  std::condition_variable cv_;
  std::array<double, kVoiceCount> desired_hz_{};
  std::array<double, kVoiceCount> sent_hz_{};
  /** Host should keep streaming / watching until card reports idle. */
  std::array<bool, kVqVoices> watch_idle_{};
  int vq_misses_ = 0;
  std::array<bool, kVqVoices> root_dirty_{};
  std::array<double, kVqVoices> pending_root_hz_{};
  IdleHandler idle_handler_;
  VqHandler vq_handler_;
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

  bool RootPending() const
  {
    for (uint8_t i = 0; i < kVqVoices; ++i) {
      if (root_dirty_[i]) {
        return true;
      }
    }
    return false;
  }

  bool TakeDirtyRoot(uint8_t &id_out, double &hz_out)
  {
    for (uint8_t i = 0; i < kVqVoices; ++i) {
      if (root_dirty_[i]) {
        id_out = i;
        hz_out = pending_root_hz_[i];
        root_dirty_[i] = false;
        return true;
      }
    }
    return false;
  }

  bool WorkPending() const
  {
    if (halted_.load()) {
      return false;
    }
    if (RootPending()) {
      return true;
    }
    for (uint8_t i = 0; i < kVoiceCount; ++i) {
      if (!HzEqual(desired_hz_[i], sent_hz_[i])) {
        return true;
      }
    }
    return false;
  }

  bool WatchPending() const
  {
    /* UART TX of a vq reply steals TinyUSB SOF time; ISO OUT drops.
     * A stale watch on another slot must not poll while a note is live. */
    for (uint8_t i = 0; i < kVoiceCount; ++i) {
      if (desired_hz_[i] > 0.0 || sent_hz_[i] > 0.0) {
        return false;
      }
    }
    for (uint8_t i = 0; i < kVqVoices; ++i) {
      if (watch_idle_[i]) {
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

  void MarkSent(uint8_t slot, double hz)
  {
    sent_hz_[slot] = hz;
    if (slot < kVqVoices) {
      watch_idle_[slot] = (hz <= 0.0);
    }
  }

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
      watch_idle_.fill(false);
      vq_misses_ = 0;
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

  void HandleVqReply(const cardproto::Result &r)
  {
    uint8_t mask = 0;
    uint8_t best = 0xFF;
    std::array<uint8_t, kVqVoices> slots{};
    if (!ParseVq(r.raw, mask, best, slots)) {
      std::fprintf(stderr, "warn: bad vq reply: %s\n",
                   r.raw[0] != '\0' ? r.raw : "(empty)");
      return;
    }
    IdleHandler handler;
    VqHandler vq_handler;
    std::array<bool, kVqVoices> become_idle{};
    {
      std::lock_guard<std::mutex> lock(mu_);
      handler = idle_handler_;
      vq_handler = vq_handler_;
      for (uint8_t i = 0; i < kVqVoices; ++i) {
        const bool active = (mask & static_cast<uint8_t>(1u << i)) != 0;
        if (active && slots[i] >= 8u) {
          std::fprintf(stderr, "warn: ring underrun voice %u\n",
                       static_cast<unsigned>(i));
        }
        if (watch_idle_[i] && !active) {
          watch_idle_[i] = false;
          become_idle[i] = true;
        }
      }
    }

    if (vq_handler) {
      vq_handler(mask, best, slots);
    }
    if (!handler) {
      return;
    }
    for (uint8_t i = 0; i < kVqVoices; ++i) {
      if (become_idle[i]) {
        handler(i);
      }
    }
  }

  void WorkerMain()
  {
    while (true) {
      uint8_t slot = 0;
      double hz = 0.0;
      uint8_t root_id = 0;
      double root_hz = 0.0;
      bool do_root = false;
      bool do_note = false;
      bool do_vq = false;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, kVqPollInterval, [&] {
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
        if (TakeDirtyRoot(root_id, root_hz)) {
          do_root = true;
        } else if (TakeDirtySlot(slot, hz)) {
          hz = desired_hz_[slot];
          if (!HzEqual(hz, sent_hz_[slot])) {
            do_note = true;
          }
        } else if (WatchPending()) {
          do_vq = true;
        }
      }

      if (do_root) {
        char cmd[48];
        std::snprintf(cmd, sizeof(cmd), "ar %u %.6g",
                      static_cast<unsigned>(root_id), root_hz);
        cardproto::Result r = bus.Channel().Exec(cmd);
        if (r.ok()) {
          std::printf("ok     ar %u %.2f Hz\n", static_cast<unsigned>(root_id),
                      root_hz);
          continue;
        }
        if (r.status == cardproto::Status::IoError ||
            r.status == cardproto::Status::Timeout) {
          TripHalt("no [C]ok for ar");
          continue;
        }
        std::fprintf(stderr, "warn: ar failed raw=%s\n",
                     r.raw[0] != '\0' ? r.raw : "(empty)");
        continue;
      }

      if (do_note) {
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

        cardproto::Result r = bus.Channel().SetNote(slot, hz);
        if (r.ok()) {
          {
            std::lock_guard<std::mutex> lock(mu_);
            MarkSent(slot, hz);
          }
          print_ok(hz);
          continue;
        }

        if (r.status == cardproto::Status::Err) {
          char msg[96];
          std::snprintf(msg, sizeof(msg), "card err:%s on n%X %.2f", r.err_code,
                        static_cast<unsigned>(slot), hz);
          TripHalt(msg);
          continue;
        }

        if (r.status == cardproto::Status::IoError) {
          TripHalt("serial I/O error (no reliable ACK path)");
          continue;
        }

        char msg[192];
        std::snprintf(msg, sizeof(msg), "no [C]ok for n%X %.2f — RX: %s",
                      static_cast<unsigned>(slot), hz,
                      r.raw[0] != '\0' ? r.raw : "(empty)");
        TripHalt(msg);
        continue;
      }

      if (do_vq) {
        cardproto::Result r = bus.Channel().QueryVoiceStatus();
        if (r.ok()) {
          vq_misses_ = 0;
          HandleVqReply(r);
          continue;
        }
        if (r.status == cardproto::Status::IoError ||
            r.status == cardproto::Status::Timeout) {
          /* Status poll only — do not halt note TX on a late USB/ISO stall. */
          ++vq_misses_;
          std::fprintf(stderr, "warn: vq timeout (%d/%d)\n", vq_misses_,
                       kVqMissLimit);
          if (vq_misses_ >= kVqMissLimit) {
            IdleHandler handler;
            std::array<bool, kVqVoices> become_idle{};
            {
              std::lock_guard<std::mutex> lock(mu_);
              handler = idle_handler_;
              for (uint8_t i = 0; i < kVqVoices; ++i) {
                if (watch_idle_[i]) {
                  watch_idle_[i] = false;
                  become_idle[i] = true;
                }
              }
              vq_misses_ = 0;
            }
            std::fprintf(stderr, "warn: vq stalled — stopped UAC watch\n");
            if (handler) {
              for (uint8_t i = 0; i < kVqVoices; ++i) {
                if (become_idle[i]) {
                  handler(i);
                }
              }
            }
          }
          continue;
        }
        /* Err / BadReply: log and keep watching; notes still work. */
        std::fprintf(stderr, "warn: vq failed status=%d raw=%s\n",
                    static_cast<int>(r.status),
                    r.raw[0] != '\0' ? r.raw : "(empty)");
      }
    }
  }

  void StartWorker()
  {
    desired_hz_.fill(0.0);
    sent_hz_.fill(0.0);
    watch_idle_.fill(false);
    vq_misses_ = 0;
    root_dirty_.fill(false);
    pending_root_hz_.fill(0.0);
    halted_ = false;
    run_ = true;
    worker_ = std::thread([this] { WorkerMain(); });
  }

  void StopWorker()
  {
    {
      std::lock_guard<std::mutex> lock(mu_);
      desired_hz_.fill(0.0);
      watch_idle_.fill(false);
      vq_misses_ = 0;
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
                           cardlink::rs485::EffectEcho effect_echo)
{
  Close();
  if (atten_db > 127u) {
    throw std::runtime_error("gain atten_db must be 0..127");
  }
  atten_db_ = atten_db;

  cardlink::rs485::BusOptions opts;
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

  const cardproto::Result r = impl_->bus.Open(serial_path, opts);
  if (!r.ok()) {
    std::string msg = "RS485 session open failed";
    if (r.raw[0] != '\0') {
      msg += ": ";
      msg += r.raw;
    } else if (r.status == cardproto::Status::Timeout) {
      msg += " (timeout — check wiring / Channel Card power)";
    } else if (r.status == cardproto::Status::Err) {
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

void ChannelRs485Out::SetIdleHandler(IdleHandler handler)
{
  if (!impl_) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->mu_);
  impl_->idle_handler_ = std::move(handler);
}

void ChannelRs485Out::SetVqHandler(VqHandler handler)
{
  if (!impl_) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->mu_);
  impl_->vq_handler_ = std::move(handler);
}

void ChannelRs485Out::SetRootHz(uint8_t wave_id, double hz)
{
  if (!impl_ || !impl_->run_.load() || impl_->halted_.load()) {
    return;
  }
  if (wave_id >= kVqVoices || !(hz > 0.0)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mu_);
    impl_->pending_root_hz_[wave_id] = hz;
    impl_->root_dirty_[wave_id] = true;
  }
  impl_->cv_.notify_one();
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
