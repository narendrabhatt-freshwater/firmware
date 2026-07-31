#include "channel_rs485.hpp"

#include "rs485_link.hpp"
#include "serial_port.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace midi_host
{
namespace
{

/** Wire: c: + 0x01 + 16×uint16 LE Hz + sum8 + CR. */
constexpr uint8_t kBinBankMagic = 0x01;
constexpr size_t kBinBankFrameLen = 37;

void SleepMs(uint32_t ms)
{
  if (ms == 0) {
    return;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool ReplyLooksOk(const std::string& reply)
{
  return reply.find("ok:") != std::string::npos;
}

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
  rs485::SerialPort port;
  std::unique_ptr<rs485::RS485Link> link;

  std::mutex mu_;
  std::condition_variable cv_;
  /**
   * Absolute desired bank: freq_hz[i] > 0 → voice on, else off.
   * Latest snapshot wins — a later sync heals any miss.
   */
  std::array<double, kVoiceCount> snap_hz_{};
  /** Last frequencies we believe the card has. */
  std::array<double, kVoiceCount> sent_hz_{};
  bool snap_dirty_ = false;
  std::atomic<bool> run_{false};
  std::thread worker_;

  bool echo_disabled_ = false;
  bool quiet_enabled_ = false;
  bool burst_notes_ = false;

  void WriteWire(const uint8_t* bytes, size_t len, bool drain)
  {
    if (port.ManualRtsControl()) {
      port.SetRts(true);
    }
    (void)port.Write(bytes, len);
    if (port.ManualRtsControl()) {
      port.SetRts(false);
    }
    // tcdrain only on full clear so the USB queue cannot hide a stale On.
    if (drain) {
      port.DrainOutput();
    }
  }

  /** One absolute binary bank frame (all 16 slots). */
  void SendBinBank(const std::array<double, kVoiceCount>& hz, bool drain)
  {
    uint8_t frame[kBinBankFrameLen];
    frame[0] = 'c';
    frame[1] = ':';
    frame[2] = kBinBankMagic;

    uint8_t sum = 0;
    for (uint8_t i = 0; i < kVoiceCount; ++i) {
      const uint16_t u = HzToU16(hz[i]);
      const uint8_t lo = static_cast<uint8_t>(u & 0xFFu);
      const uint8_t hi = static_cast<uint8_t>((u >> 8) & 0xFFu);
      frame[3u + (2u * i)] = lo;
      frame[3u + (2u * i) + 1u] = hi;
      sum = static_cast<uint8_t>(sum + lo + hi);
    }
    frame[35] = sum;
    frame[36] = '\r';

    port.FlushInput();
    WriteWire(frame, kBinBankFrameLen, drain);
  }

  /**
   * Absolute bank sync: one binary frame. Quiet mode is fire-and-forget, so
   * a single dropped Off leaves the card singing while we believe sent_hz_.
   * Retransmit whenever any slot turns off (and always drain on full clear).
   */
  void SendSnapshot(const std::array<double, kVoiceCount>& hz)
  {
    if (hz == sent_hz_) {
      return;
    }

    bool any_on = false;
    bool any_release = false;
    for (uint8_t i = 0; i < kVoiceCount; ++i) {
      if (hz[i] > 0.0) {
        any_on = true;
      }
      if (hz[i] <= 0.0 && sent_hz_[i] > 0.0) {
        any_release = true;
      }
    }

    const bool drain = !any_on;
    SendBinBank(hz, drain);
    if (any_release) {
      // Second copy heals one lost Off without waiting for the next key.
      SendBinBank(hz, drain);
    }
    sent_hz_ = hz;
  }

  void PublishBank(const VoiceBank& bank)
  {
    {
      std::lock_guard<std::mutex> lock(mu_);
      const auto& slots = bank.Slots();
      for (uint8_t i = 0; i < kVoiceCount; ++i) {
        snap_hz_[i] =
            (slots[i].active && slots[i].freq_hz > 0.0) ? slots[i].freq_hz
                                                         : 0.0;
      }
      snap_dirty_ = true;
    }
    cv_.notify_one();
  }

  void WorkerMain()
  {
    while (true) {
      std::array<double, kVoiceCount> local{};
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] { return snap_dirty_ || !run_.load(); });
        if (!run_.load() && !snap_dirty_) {
          return;
        }
        // No coalesce sleep — send as soon as a bank changes. Notes that
        // arrive during TX still update snap_hz_ and flush on the next loop.
        local = snap_hz_;
        snap_dirty_ = false;
      }
      SendSnapshot(local);
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
      snap_dirty_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void EnsureLink()
  {
    if (link) {
      return;
    }
    rs485::LinkOptions opts;
    opts.reply_timeout_ms = 300;
    opts.idle_gap_ms = 50;
    opts.retries = 1;
    opts.inter_byte_ms = 8;
    link = std::make_unique<rs485::RS485Link>(port, opts);
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
  impl_->echo_disabled_ = false;
  impl_->quiet_enabled_ = false;
  impl_->burst_notes_ = false;
  impl_->snap_hz_.fill(0.0);
  impl_->sent_hz_.fill(0.0);
  impl_->snap_dirty_ = false;

  if (!impl_->port.Open(serial_path, baud)) {
    throw std::runtime_error("RS485 open failed: " + impl_->port.LastError());
  }
  path_ = serial_path;

  impl_->EnsureLink();

  // Ctrl+C / kill skips Close(), which leaves c:quiet on armed — and quiet
  // mutes every RS485 reply, so the next session's n0 looks like a dead bus.
  (void)impl_->link->Send(rs485::Target::Channel, "quiet off");

  const rs485::ExchangeResult init =
      impl_->link->Send(rs485::Target::Channel, "n0");
  if (!init.got_reply) {
    throw std::runtime_error(
        "no reply to session n0 — check RS485 wiring / Channel Card power "
        "(or flash Channel with quiet on|off + bare-n0 quiet clear)");
  }

  char gain_cmd[32];
  std::snprintf(gain_cmd, sizeof(gain_cmd), "gain 1 %u", atten_db_);
  const rs485::ExchangeResult gain =
      impl_->link->Send(rs485::Target::Channel, gain_cmd);
  if (!gain.got_reply) {
    throw std::runtime_error(std::string("no reply to ") + gain_cmd);
  }

  static const char* const kSlots = "0123456789abcdef";
  for (uint8_t i = 0; i < 16; ++i) {
    char cmd[8];
    std::snprintf(cmd, sizeof(cmd), "n%c 0", kSlots[i]);
    (void)impl_->link->Send(rs485::Target::Channel, cmd);
  }

  const rs485::ExchangeResult quiet =
      impl_->link->Send(rs485::Target::Channel, "quiet on");
  impl_->quiet_enabled_ = quiet.got_reply && ReplyLooksOk(quiet.reply);

  const rs485::ExchangeResult echo_off =
      impl_->link->Send(rs485::Target::Effect, "echo off");
  impl_->echo_disabled_ =
      echo_off.got_reply && ReplyLooksOk(echo_off.reply);
  impl_->burst_notes_ = true;

  impl_->link.reset();
  impl_->StartWorker();
}

void ChannelRs485Out::Close()
{
  if (!impl_) {
    return;
  }
  if (impl_->run_.load()) {
    {
      std::lock_guard<std::mutex> lock(impl_->mu_);
      impl_->snap_hz_.fill(0.0);
      impl_->snap_dirty_ = true;
    }
    impl_->cv_.notify_one();
    SleepMs(80);
    impl_->StopWorker();
  }

  if (impl_->port.IsOpen() && impl_->quiet_enabled_) {
    impl_->EnsureLink();
    (void)impl_->link->Send(rs485::Target::Channel, "quiet off");
    impl_->quiet_enabled_ = false;
  }

  impl_->link.reset();
  impl_->echo_disabled_ = false;
  impl_->burst_notes_ = false;
  impl_->port.Close();
  path_.clear();
}

void ChannelRs485Out::ApplyBankEvent(const BankEvent& event,
                                     const VoiceBank& bank)
{
  if (!impl_ || !impl_->run_.load()) {
    return;
  }
  if (event.kind == BankEventKind::Steal) {
    return;
  }
  impl_->PublishBank(bank);
}

bool ChannelRs485Out::BurstNotes() const
{
  return impl_ && impl_->burst_notes_;
}

bool ChannelRs485Out::EffectEchoDisabled() const
{
  return impl_ && impl_->echo_disabled_;
}

bool ChannelRs485Out::QuietReplies() const
{
  return impl_ && impl_->quiet_enabled_;
}

} // namespace midi_host
