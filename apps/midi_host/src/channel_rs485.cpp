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
#include <string>
#include <thread>
#include <vector>

namespace midi_host
{
namespace
{

/** Fixed per-voice scale: ~1/8 FS so chords get louder additively. */
constexpr double kVoiceScale = 0.125;

/** Inter-frame gaps (µs) with Effect echo off + quiet on. */
constexpr uint32_t kGapOnUs = 100;
constexpr uint32_t kGapOffUs = 150;
constexpr uint32_t kGapClearUs = 300;

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

bool ReplyLooksOk(const std::string& reply)
{
  return reply.find("ok:") != std::string::npos;
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
   * Incremental On/Off over a lossy bus left stuck sines whenever an Off
   * frame was dropped. Latest snapshot wins — a later sync heals any miss.
   */
  std::array<double, kVoiceCount> snap_hz_{};
  /** Last frequencies we believe the card has (for delta sync). */
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
    // tcdrain on every USB-serial frame makes a 16-key chord feel hung
    // (each drain can cost tens of ms). Only use it when we must not leave
    // a stale chord queued behind a clear.
    if (drain) {
      port.DrainOutput();
    }
  }

  static std::string SlotOffCommand(uint8_t slot)
  {
    const char hex = "0123456789abcdef"[slot & 0x0Fu];
    char cmd[8];
    std::snprintf(cmd, sizeof(cmd), "n%c 0", hex);
    return std::string(cmd);
  }

  static std::string SlotOnCommand(uint8_t slot, double hz)
  {
    const char hex = "0123456789abcdef"[slot & 0x0Fu];
    char cmd[32];
    std::snprintf(cmd, sizeof(cmd), "n%c %.0f %.2f", hex, hz, kVoiceScale);
    return std::string(cmd);
  }

  void SendOneFrame(const std::string& body, bool drain)
  {
    const std::string wire = std::string("c:") + body + "\r";
    port.FlushInput();
    WriteWire(reinterpret_cast<const uint8_t*>(wire.data()), wire.size(),
              drain);
  }

  /**
   * Delta sync at wire speed. Full release drains the last frame so clears
   * land even under 16-voice audio load.
   */
  void SendSnapshot(const std::array<double, kVoiceCount>& hz)
  {
    auto emit = [&](const std::string& body, uint32_t gap_us, bool drain) {
      SendOneFrame(body, drain);
      if (gap_us > 0) {
        SleepUs(gap_us);
      }
    };

    bool any_on = false;
    for (uint8_t i = 0; i < kVoiceCount; ++i) {
      if (hz[i] > 0.0) {
        any_on = true;
        break;
      }
    }

    if (!any_on) {
      // Full clear: one silence, then every slot off. Drain only the last
      // frame so the USB queue cannot hide a stale On behind us.
      emit("silence", kGapClearUs, false);
      for (uint8_t i = 0; i < kVoiceCount; ++i) {
        const bool last = (i + 1u == kVoiceCount);
        emit(SlotOffCommand(i), last ? 0 : kGapClearUs, last);
      }
      emit("silence", 0, true);
      sent_hz_.fill(0.0);
      return;
    }

    for (uint8_t i = 0; i < kVoiceCount; ++i) {
      if (sent_hz_[i] > 0.0 && hz[i] <= 0.0) {
        emit(SlotOffCommand(i), kGapOffUs, false);
        sent_hz_[i] = 0.0;
      }
    }
    for (uint8_t i = 0; i < kVoiceCount; ++i) {
      if (hz[i] > 0.0 && hz[i] != sent_hz_[i]) {
        emit(SlotOnCommand(i, hz[i]), kGapOnUs, false);
        sent_hz_[i] = hz[i];
      }
    }
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
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] { return snap_dirty_ || !run_.load(); });
        if (!run_.load() && !snap_dirty_) {
          return;
        }
      }
      // Short coalesce so a 16-key mash is one delta, not 16 bus trips.
      SleepMs(2);

      std::array<double, kVoiceCount> local{};
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (!run_.load() && !snap_dirty_) {
          return;
        }
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
  // quiet off clears the flag before TX, so this recovers without a power cycle.
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

  // Effect defaults to echo off; still request it for older firmware. Note
  // TX always bursts — paced per-byte fallback is gone.
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
    SleepMs(150);
    impl_->StopWorker();
  }

  // Leave Effect echo off (firmware default). Only clear Channel quiet so
  // the next fw rs485 console session still gets replies.
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
  // Steal is followed by On for the same slot; sync once on On/Off/Retrig.
  if (event.kind == BankEventKind::Steal) {
    return;
  }
  // Absolute sync: push the whole bank, not a single delta. A lost Off on
  // the previous frame is corrected the next time any key moves.
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
