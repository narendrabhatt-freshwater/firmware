#include "channel_rs485.hpp"

#include "rs485_link.hpp"
#include "serial_port.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace midi_host
{
namespace
{

/** Visible on scope with gain 1 6; soft-clips if many notes at once. */
constexpr double kVoiceScale = 0.5;

void SleepMs(uint32_t ms)
{
  if (ms == 0) {
    return;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void SleepUs(uint32_t us)
{
  if (us == 0) {
    return;
  }
  std::this_thread::sleep_for(std::chrono::microseconds(us));
}

/** Read until the RX line is quiet for quiet_ms, or overall_ms elapses.
 * Channel replies to every nX; we must finish draining that reply before
 * the next TX or half-duplex collisions corrupt / drop note-offs. */
void DrainUntilQuiet(rs485::SerialPort& port,
                     uint32_t overall_ms,
                     uint32_t quiet_ms)
{
  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + std::chrono::milliseconds(overall_ms);
  uint8_t trash[64];

  while (clock::now() < deadline) {
    if (port.ReadTimeout(trash, sizeof(trash), quiet_ms) == 0) {
      return;
    }
  }
}

} // namespace

struct ChannelRs485Out::Impl
{
  rs485::SerialPort port;
  std::unique_ptr<rs485::RS485Link> link;
  /** Inter-byte TX delay (µs). 0 is treated as 1000 in SendCommand. */
  uint32_t pace_us = 1000;

  std::mutex mu_;
  std::condition_variable cv_;
  /** Latest pending command per voice slot; empty string = nothing queued.
   * Coalescing means a release always replaces a pending press for that
   * slot — backlog cannot drop offs while keeping stale ons. */
  std::array<std::string, kVoiceCount> pending_{};
  std::atomic<bool> run_{false};
  std::thread worker_;

  bool AnyPendingLocked() const
  {
    for (const std::string& p : pending_) {
      if (!p.empty()) {
        return true;
      }
    }
    return false;
  }

  /** Take one pending command (lowest slot index). Empty if none. */
  std::string TakeOneLocked()
  {
    for (uint8_t i = 0; i < kVoiceCount; ++i) {
      if (!pending_[i].empty()) {
        std::string cmd = std::move(pending_[i]);
        pending_[i].clear();
        return cmd;
      }
    }
    return std::string();
  }

  void SendCommand(const std::string& body)
  {
    // Match apps/console RS485Link pacing: Effect Card echoes every
    // keystroke onto the shared bus. A burst write overlaps that echo and
    // corrupts the frame on Channel (fw rs485 send works; pace-0 MIDI did
    // not). Default pacing drains between bytes; after the final CR, wait
    // for Channel's reply without eating it mid-flight.
    const std::string wire = std::string("c:") + body + "\r";
    port.FlushInput();
    const auto* bytes = reinterpret_cast<const uint8_t*>(wire.data());

    if (pace_us == 0u) {
      if (port.ManualRtsControl()) {
        port.SetRts(true);
      }
      (void)port.Write(bytes, wire.size());
      if (port.ManualRtsControl()) {
        port.SetRts(false);
      }
    } else {
      for (size_t i = 0; i < wire.size(); ++i) {
        if (port.ManualRtsControl()) {
          port.SetRts(true);
        }
        if (!port.Write(bytes + i, 1)) {
          if (port.ManualRtsControl()) {
            port.SetRts(false);
          }
          return;
        }
        if (port.ManualRtsControl()) {
          port.SetRts(false);
        }
        const bool last = (i + 1u == wire.size());
        if (!last) {
          SleepUs(pace_us);
          DrainUntilQuiet(port, (pace_us + 999u) / 1000u + 2u, 2);
        }
      }
    }

    SleepMs(2);
    DrainUntilQuiet(port, 40, 3);
  }

  void Enqueue(uint8_t slot, std::string cmd)
  {
    if (slot >= kVoiceCount) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      pending_[slot] = std::move(cmd);
    }
    cv_.notify_one();
  }

  void WorkerMain()
  {
    while (true) {
      std::string cmd;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] { return AnyPendingLocked() || !run_.load(); });
        if (!run_.load() && !AnyPendingLocked()) {
          return;
        }
        cmd = TakeOneLocked();
      }
      if (!cmd.empty()) {
        SendCommand(cmd);
      }
    }
  }

  void StartWorker()
  {
    run_ = true;
    worker_ = std::thread([this] { WorkerMain(); });
  }

  void StopWorker()
  {
    {
      std::lock_guard<std::mutex> lock(mu_);
      run_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
    std::lock_guard<std::mutex> lock(mu_);
    for (std::string& p : pending_) {
      p.clear();
    }
  }
};

ChannelRs485Out::ChannelRs485Out()
    : impl_(std::make_unique<Impl>())
{
}

ChannelRs485Out::~ChannelRs485Out()
{
  Close();
}

void ChannelRs485Out::Open(const std::string& serial_path,
                           uint32_t baud,
                           uint32_t atten_db,
                           uint32_t pace_us)
{
  Close();
  if (atten_db > 127u) {
    throw std::runtime_error("gain atten_db must be 0..127");
  }
  atten_db_ = atten_db;
  pace_us_ = pace_us;
  impl_->pace_us = pace_us;

  if (!impl_->port.Open(serial_path, baud)) {
    throw std::runtime_error("RS485 open failed: " + impl_->port.LastError());
  }
  path_ = serial_path;

  // Session setup still uses the reliable request/reply path (once).
  rs485::LinkOptions opts;
  opts.reply_timeout_ms = 300;
  opts.idle_gap_ms = 50;
  opts.retries = 1;
  opts.inter_byte_ms = 8;
  impl_->link = std::make_unique<rs485::RS485Link>(impl_->port, opts);

  const rs485::ExchangeResult init =
      impl_->link->Send(rs485::Target::Channel, "n0");
  if (!init.got_reply) {
    throw std::runtime_error(
        "no reply to session n0 — check RS485 wiring / Channel Card power");
  }

  char gain_cmd[32];
  std::snprintf(gain_cmd, sizeof(gain_cmd), "gain 1 %u", atten_db_);
  const rs485::ExchangeResult gain =
      impl_->link->Send(rs485::Target::Channel, gain_cmd);
  if (!gain.got_reply) {
    throw std::runtime_error(std::string("no reply to ") + gain_cmd);
  }

  // Clear leftover tones synchronously before the worker starts.
  static const char* const kSlots = "0123456789abcdef";
  for (uint8_t i = 0; i < 16; ++i) {
    char cmd[8];
    std::snprintf(cmd, sizeof(cmd), "n%c 0", kSlots[i]);
    (void)impl_->link->Send(rs485::Target::Channel, cmd);
  }

  // Note traffic: background TX, MIDI thread never waits.
  impl_->link.reset();
  impl_->StartWorker();
}

void ChannelRs485Out::Close()
{
  if (!impl_) {
    return;
  }
  if (impl_->run_.load()) {
    // Best-effort silence before stopping the worker.
    static const char* const kSlots = "0123456789abcdef";
    for (uint8_t i = 0; i < 16; ++i) {
      char cmd[8];
      std::snprintf(cmd, sizeof(cmd), "n%c 0", kSlots[i]);
      impl_->Enqueue(i, cmd);
    }
    // Give the worker a moment to drain.
    SleepMs(200);
    impl_->StopWorker();
  }
  impl_->port.Close();
  path_.clear();
}

void ChannelRs485Out::ApplyBankEvent(const BankEvent& event,
                                     const VoiceBank& /*bank*/)
{
  if (!impl_ || !impl_->run_.load()) {
    return;
  }

  const uint8_t slot = static_cast<uint8_t>(event.slot & 0x0Fu);
  const char slot_hex = "0123456789abcdef"[slot];
  char cmd[64];

  switch (event.kind) {
  case BankEventKind::On:
  case BankEventKind::Retrig:
    // Every byte costs ~1 ms of pacing, and the card quantises to 0.1 Hz
    // in its own reply — keep the frame short rather than 4 decimals.
    std::snprintf(cmd, sizeof(cmd), "n%c %.1f %.2f", slot_hex, event.freq_hz,
                  kVoiceScale);
    break;
  case BankEventKind::Off:
  case BankEventKind::Steal:
    std::snprintf(cmd, sizeof(cmd), "n%c 0", slot_hex);
    break;
  }

  // Steal then On for the same slot coalesce to just On — one bus trip.
  impl_->Enqueue(slot, cmd);
}

} // namespace midi_host
