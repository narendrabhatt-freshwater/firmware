#include "channel_rs485.hpp"

#include "rs485_link.hpp"
#include "serial_port.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <queue>
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

} // namespace

struct ChannelRs485Out::Impl
{
  rs485::SerialPort port;
  std::unique_ptr<rs485::RS485Link> link;

  std::mutex mu_;
  std::condition_variable cv_;
  std::queue<std::string> q_;
  std::atomic<bool> run_{false};
  std::thread worker_;

  void SendPaced(const std::string& body)
  {
    // "c:" + body + "\r" — paced bytes, drain echo briefly, no reply wait.
    const std::string wire = std::string("c:") + body + "\r";
    port.FlushInput();
    const auto* bytes = reinterpret_cast<const uint8_t*>(wire.data());
    for (size_t i = 0; i < wire.size(); ++i) {
      if (port.ManualRtsControl()) {
        port.SetRts(true);
      }
      (void)port.Write(bytes + i, 1);
      if (port.ManualRtsControl()) {
        port.SetRts(false);
      }
      SleepMs(1);
      if (i + 1 < wire.size()) {
        uint8_t trash[32];
        (void)port.ReadTimeout(trash, sizeof(trash), 2);
      }
    }
    SleepMs(3);
    uint8_t trash[64];
    (void)port.ReadTimeout(trash, sizeof(trash), 5);
  }

  void Enqueue(std::string cmd)
  {
    {
      std::lock_guard<std::mutex> lock(mu_);
      // Bound latency: if the player outruns the bus, drop oldest notes
      // rather than building multi-second backlog.
      constexpr size_t kMaxQueued = 32;
      while (q_.size() >= kMaxQueued) {
        q_.pop();
      }
      q_.push(std::move(cmd));
    }
    cv_.notify_one();
  }

  void WorkerMain()
  {
    while (true) {
      std::string cmd;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] { return !q_.empty() || !run_.load(); });
        if (!run_.load() && q_.empty()) {
          return;
        }
        cmd = std::move(q_.front());
        q_.pop();
      }
      SendPaced(cmd);
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
    while (!q_.empty()) {
      q_.pop();
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
                           uint32_t atten_db)
{
  Close();
  if (atten_db > 127u) {
    throw std::runtime_error("gain atten_db must be 0..127");
  }
  atten_db_ = atten_db;

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

  // Note traffic: background paced TX, MIDI thread never waits.
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
      impl_->Enqueue(cmd);
    }
    // Give the worker a moment to drain.
    SleepMs(50);
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

  char cmd[64];
  const char slot_hex = "0123456789abcdef"[event.slot & 0x0Fu];

  switch (event.kind) {
  case BankEventKind::On:
  case BankEventKind::Retrig:
    std::snprintf(cmd, sizeof(cmd), "n%c %.4f %.4f", slot_hex, event.freq_hz,
                  kVoiceScale);
    break;
  case BankEventKind::Off:
  case BankEventKind::Steal:
    std::snprintf(cmd, sizeof(cmd), "n%c 0", slot_hex);
    break;
  }

  impl_->Enqueue(cmd);
}

} // namespace midi_host
