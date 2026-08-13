#include "bus_controller.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace
{

constexpr unsigned kVqVoices = 8;
/* Release envelopes are tens of ms to seconds. 10 ms hammered the half-duplex
 * bus during USB ISO (blocking [C]ok:vq TX) and a single late poll was treated
 * as a fatal fault + Soft Recover. */
constexpr auto kVqPollInterval = std::chrono::milliseconds(50);
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

void LogResult(const std::function<void(const std::string &)> &push,
               cardproto::Target target,
               const cardproto::Result &r)
{
  const char *tag = (target == cardproto::Target::Effect)    ? "[E] "
                    : (target == cardproto::Target::Channel) ? "[C] "
                                                         : "[*] ";
  if (r.status == cardproto::Status::Ok) {
    push(std::string(tag) + (r.raw[0] ? r.raw : "ok"));
  } else {
    push(std::string(tag) + "err: " + (r.raw[0] ? r.raw : "timeout/io"));
  }
}

/** Parse `ok:vq <mask_hex> q0..q7` from Result::raw. */
bool ParseVq(const char *raw, uint8_t &mask_out, std::array<uint8_t, kVqVoices> &q_out)
{
  if (raw == nullptr) {
    return false;
  }
  const char *p = raw;
  if (std::strncmp(p, "ok:", 3) == 0) {
    p += 3;
  }
  while (*p == ' ') {
    ++p;
  }
  if (std::strncmp(p, "vq", 2) == 0) {
    p += 2;
  }
  unsigned mask = 0;
  unsigned q[kVqVoices] = {};
  const int n = std::sscanf(p, " %x %u %u %u %u %u %u %u %u", &mask, &q[0],
                            &q[1], &q[2], &q[3], &q[4], &q[5], &q[6], &q[7]);
  if (n != 1 + static_cast<int>(kVqVoices) || mask > 0xffu) {
    return false;
  }
  for (unsigned i = 0; i < kVqVoices; ++i) {
    if (q[i] > 4u) {
      return false;
    }
    q_out[i] = static_cast<uint8_t>(q[i]);
  }
  mask_out = static_cast<uint8_t>(mask);
  return true;
}

} // namespace

struct BusController::Impl
{
  cardlink::rs485::Bus bus;

  std::mutex mu_;
  std::condition_variable cv_;
  std::array<double, midi_host::kVoiceCount> desired_hz_{};
  std::array<double, midi_host::kVoiceCount> sent_hz_{};
  std::array<bool, kVqVoices> watch_idle_{};
  std::array<bool, kVqVoices> underrun_logged_{};
  int vq_misses_ = 0;
  IdleHandler idle_handler_;
  std::queue<Job> jobs_;
  std::vector<UiMirrorPatch> ui_mirror_;
  std::atomic<bool> run_{false};
  std::thread worker_;
  std::thread open_thread_;
  LogBuffer *log_ = nullptr;

  bool WorkPendingLocked() const
  {
    for (uint8_t i = 0; i < midi_host::kVoiceCount; ++i) {
      if (!HzEqual(desired_hz_[i], sent_hz_[i])) {
        return true;
      }
    }
    return false;
  }

  bool WatchPendingLocked() const
  {
    for (uint8_t i = 0; i < kVqVoices; ++i) {
      if (watch_idle_[i]) {
        return true;
      }
    }
    return false;
  }

  void MarkNoteSentLocked(uint8_t slot, double hz)
  {
    sent_hz_[slot] = hz;
    if (slot < kVqVoices) {
      /* Poll only after note-off, until the card finishes release. */
      watch_idle_[slot] = (hz <= 0.0);
      underrun_logged_[slot] = false;
    }
  }

  void ClearWatchLocked()
  {
    watch_idle_.fill(false);
    underrun_logged_.fill(false);
    vq_misses_ = 0;
  }
};

BusController::BusController() : impl_(std::make_unique<Impl>()) {}

BusController::~BusController()
{
  LogBuffer sink;
  if (impl_ && impl_->open_thread_.joinable()) {
    impl_->open_thread_.join();
  }
  if (open_.load()) {
    Close(sink);
  }
}

std::string BusController::Path() const
{
  return impl_ ? impl_->bus.Path() : std::string{};
}

uint32_t BusController::TimeoutCount() const
{
  return impl_ ? impl_->bus.TimeoutCount() : 0;
}

uint32_t BusController::ErrCount() const
{
  return impl_ ? impl_->bus.ErrCount() : 0;
}

std::size_t BusController::QueueDepth() const
{
  if (!impl_) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(impl_->mu_);
  return impl_->jobs_.size();
}

std::vector<UiMirrorPatch> BusController::DrainUiMirror()
{
  std::vector<UiMirrorPatch> out;
  if (!impl_) {
    return out;
  }
  std::lock_guard<std::mutex> lock(impl_->mu_);
  out.swap(impl_->ui_mirror_);
  return out;
}

void BusController::AcknowledgeSlotHz(uint8_t slot, double hz)
{
  if (!impl_ || slot >= midi_host::kVoiceCount) {
    return;
  }
  const double v = ClampNoteHz(hz);
  std::lock_guard<std::mutex> lock(impl_->mu_);
  impl_->desired_hz_[slot] = v;
  impl_->sent_hz_[slot] = v;
  if (slot < kVqVoices) {
    impl_->watch_idle_[slot] = (v <= 0.0);
    impl_->underrun_logged_[slot] = false;
  }
}

void BusController::AcknowledgeAllHz(double hz)
{
  if (!impl_) {
    return;
  }
  const double v = ClampNoteHz(hz);
  std::lock_guard<std::mutex> lock(impl_->mu_);
  for (uint8_t i = 0; i < midi_host::kVoiceCount; ++i) {
    impl_->desired_hz_[i] = v;
    impl_->sent_hz_[i] = v;
  }
  /* Silence (hz==0): watch until card idle so UAC dry can stop. */
  for (uint8_t i = 0; i < kVqVoices; ++i) {
    impl_->watch_idle_[i] = (v <= 0.0);
    impl_->underrun_logged_[i] = false;
  }
}

void BusController::SetIdleHandler(IdleHandler handler)
{
  if (!impl_) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->mu_);
  impl_->idle_handler_ = std::move(handler);
}

void BusController::SetPollLog(LogBuffer *poll_log)
{
  poll_log_ = poll_log;
}

BusQueueResult BusController::Enqueue(Job job)
{
  if (connecting_.load()) {
    return BusQueueResult::Connecting;
  }
  if (!open_.load()) {
    return BusQueueResult::Closed;
  }
  if (halted_.load()) {
    return BusQueueResult::Halted;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mu_);
    impl_->jobs_.push(std::move(job));
  }
  impl_->cv_.notify_one();
  return BusQueueResult::Ok;
}

bool BusController::Open(const std::string &path,
                         uint32_t baud,
                         uint32_t atten_db,
                         LogBuffer &log)
{
  if (open_.load()) {
    Close(log);
  }

  cardlink::rs485::BusOptions opts;
  opts.baud = baud;
  opts.atten_db = atten_db;
  opts.effect_echo = cardlink::rs485::EffectEcho::Off;
  opts.allow_missing_effect = true;
  opts.idle_gap_ms = 0;
  opts.post_ack_settle_ms = 0;
  opts.reply_timeout_ms = 400;
  opts.retries = 1;

  const auto r = impl_->bus.Open(path, opts);
  if (r.status != cardproto::Status::Ok) {
    log.Push(std::string("err: open ") + path + " — " +
             (r.raw[0] ? r.raw : "failed"));
    return false;
  }

  atten_db_.store(atten_db);
  halted_.store(false);
  impl_->log_ = &log;
  {
    std::lock_guard<std::mutex> lock(impl_->mu_);
    impl_->desired_hz_.fill(0.0);
    impl_->sent_hz_.fill(0.0);
    impl_->ClearWatchLocked();
    while (!impl_->jobs_.empty()) {
      impl_->jobs_.pop();
    }
  }

  impl_->run_.store(true);
  impl_->worker_ = std::thread([this, &log] { WorkerMain(&log); });
  open_.store(true);
  log.Push(std::string("ok: connected ") + path + " gain " +
           std::to_string(atten_db) + " dB");
  return true;
}

void BusController::RequestOpen(const std::string &path,
                                uint32_t baud,
                                uint32_t atten_db,
                                LogBuffer &log)
{
  if (connecting_.exchange(true)) {
    log.Push("err: bus connect already in progress");
    return;
  }
  if (impl_->open_thread_.joinable()) {
    impl_->open_thread_.join();
  }
  impl_->open_thread_ = std::thread([this, path, baud, atten_db, &log] {
    (void)Open(path, baud, atten_db, log);
    connecting_.store(false);
  });
}

void BusController::Close(LogBuffer &log)
{
  if (impl_->open_thread_.joinable()) {
    impl_->open_thread_.join();
  }
  connecting_.store(false);
  if (!open_.exchange(false)) {
    return;
  }
  impl_->run_.store(false);
  impl_->cv_.notify_all();
  if (impl_->worker_.joinable()) {
    impl_->worker_.join();
  }
  impl_->bus.Close();
  impl_->log_ = nullptr;
  halted_.store(false);
  {
    std::lock_guard<std::mutex> lock(impl_->mu_);
    impl_->ClearWatchLocked();
  }
  log.Push("ok: disconnected");
}

void BusController::PublishBank(const midi_host::VoiceBank &bank)
{
  if (!open_.load() || halted_.load()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mu_);
    const auto &slots = bank.Slots();
    for (uint8_t i = 0; i < midi_host::kVoiceCount; ++i) {
      impl_->desired_hz_[i] =
          (slots[i].active && slots[i].freq_hz > 0.0)
              ? ClampNoteHz(slots[i].freq_hz)
              : 0.0;
    }
    Job j;
    j.kind = JobKind::Notes;
    impl_->jobs_.push(std::move(j));
  }
  impl_->cv_.notify_one();
}

void BusController::RequestSilence()
{
  if (!open_.load()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mu_);
    impl_->desired_hz_.fill(0.0);
    impl_->ClearWatchLocked();
    Job j;
    j.kind = JobKind::Silence;
    impl_->jobs_.push(std::move(j));
  }
  impl_->cv_.notify_one();
}

void BusController::RequestRecover(LogBuffer &log)
{
  if (!open_.load()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mu_);
    Job j;
    j.kind = JobKind::Recover;
    impl_->jobs_.push(std::move(j));
  }
  impl_->cv_.notify_one();
  log.Push("… soft recover queued");
}

BusQueueResult BusController::QueueExec(cardproto::Target target,
                                        std::string command)
{
  Job j;
  j.kind = JobKind::Exec;
  j.target = target;
  j.command = std::move(command);
  return Enqueue(std::move(j));
}

BusQueueResult BusController::QueueChannel(ChannelOp op)
{
  Job j;
  j.kind = JobKind::Channel;
  j.channel_op = std::move(op);
  return Enqueue(std::move(j));
}

BusQueueResult BusController::QueueEffect(EffectOp op)
{
  Job j;
  j.kind = JobKind::Effect;
  j.effect_op = std::move(op);
  return Enqueue(std::move(j));
}

BusQueueResult BusController::QueuePlayWave(uint8_t slot, double rate_hz)
{
  return QueueChannel([slot, rate_hz](cardproto::ChannelClient &ch) {
    return ch.PlayWave(slot, rate_hz);
  });
}

BusQueueResult BusController::QueueStopWave(uint8_t slot)
{
  return QueueChannel([slot](cardproto::ChannelClient &ch) {
    return ch.StopWave(slot);
  });
}

BusQueueResult BusController::QueueGain(uint8_t atten_db)
{
  if (connecting_.load()) {
    return BusQueueResult::Connecting;
  }
  if (!open_.load()) {
    return BusQueueResult::Closed;
  }
  if (halted_.load()) {
    return BusQueueResult::Halted;
  }
  atten_db_.store(atten_db);
  Job j;
  j.kind = JobKind::Gain;
  j.atten_db = atten_db;
  return Enqueue(std::move(j));
}

void BusController::WorkerMain(LogBuffer *log)
{
  auto push = [&](const std::string &s) {
    if (log) {
      log->Push(s);
    }
  };
  auto push_poll = [&](const std::string &s) {
    if (poll_log_) {
      poll_log_->Push(s);
    } else if (log) {
      log->Push(s);
    }
  };

  auto handle_vq = [&](const cardproto::Result &r) {
    uint8_t mask = 0;
    std::array<uint8_t, kVqVoices> quarters{};
    if (!ParseVq(r.raw, mask, quarters)) {
      push_poll(std::string("warn: bad vq reply: ") +
                (r.raw[0] ? r.raw : "(empty)"));
      return;
    }

    IdleHandler handler;
    std::array<bool, kVqVoices> become_idle{};
    {
      std::lock_guard<std::mutex> lock(impl_->mu_);
      handler = impl_->idle_handler_;
      for (uint8_t i = 0; i < kVqVoices; ++i) {
        const bool active = (mask & static_cast<uint8_t>(1u << i)) != 0;
        if (impl_->watch_idle_[i] && active && quarters[i] == 0u) {
          if (!impl_->underrun_logged_[i]) {
            push_poll("warn: ring underrun voice " + std::to_string(i));
            impl_->underrun_logged_[i] = true;
          }
        } else if (quarters[i] > 0u) {
          impl_->underrun_logged_[i] = false;
        }
        if (impl_->watch_idle_[i] && !active) {
          impl_->watch_idle_[i] = false;
          become_idle[i] = true;
        }
      }
    }

    if (!handler) {
      return;
    }
    for (uint8_t i = 0; i < kVqVoices; ++i) {
      if (become_idle[i]) {
        push_poll("ok: voice " + std::to_string(i) + " idle → silence UAC");
        handler(i);
      }
    }
  };

  while (true) {
    Job job;
    bool have_job = false;
    uint8_t note_slot = 0;
    double note_hz = 0.0;
    bool have_note = false;
    bool do_vq = false;

    {
      std::unique_lock<std::mutex> lock(impl_->mu_);
      impl_->cv_.wait_for(lock, kVqPollInterval, [&] {
        if (!impl_->run_.load()) {
          return true;
        }
        if (!impl_->jobs_.empty()) {
          return true;
        }
        if (!halted_.load() && impl_->WorkPendingLocked()) {
          return true;
        }
        return false;
      });

      if (!impl_->run_.load()) {
        break;
      }

      if (!impl_->jobs_.empty()) {
        job = std::move(impl_->jobs_.front());
        impl_->jobs_.pop();
        have_job = true;
      } else if (!halted_.load()) {
        for (uint8_t i = 0; i < midi_host::kVoiceCount; ++i) {
          if (impl_->desired_hz_[i] <= 0.0 && impl_->sent_hz_[i] > 0.0) {
            note_slot = i;
            note_hz = 0.0;
            have_note = true;
            break;
          }
        }
        if (!have_note) {
          for (uint8_t i = 0; i < midi_host::kVoiceCount; ++i) {
            if (!HzEqual(impl_->desired_hz_[i], impl_->sent_hz_[i])) {
              note_slot = i;
              note_hz = impl_->desired_hz_[i];
              have_note = true;
              break;
            }
          }
        }
        if (!have_note && impl_->WatchPendingLocked()) {
          do_vq = true;
        }
      }
    }

    if (have_job) {
      switch (job.kind) {
      case JobKind::Notes:
        break;
      case JobKind::Exec: {
        const auto r = impl_->bus.Exec(
            static_cast<cardproto::Target>(job.target), job.command);
        LogResult(push, job.target, r);
        if (r.ok()) {
          UiMirrorPatch patch =
              ParseConsoleMirror(job.target, job.command, r);
          if (patch.Any()) {
            if (patch.has_gain_db) {
              atten_db_.store(static_cast<uint32_t>(
                  std::clamp(patch.gain_db, 0, 127)));
            }
            std::lock_guard<std::mutex> lock(impl_->mu_);
            impl_->ui_mirror_.push_back(std::move(patch));
          }
        }
        if (r.status == cardproto::Status::Timeout) {
          halted_.store(true);
          push("*** bus fault — Soft Recover or reconnect");
        }
        break;
      }
      case JobKind::Channel: {
        if (job.channel_op) {
          const auto r = job.channel_op(impl_->bus.Channel());
          LogResult(push, cardproto::Target::Channel, r);
          if (r.status == cardproto::Status::Timeout) {
            halted_.store(true);
            push("*** bus fault — Soft Recover or reconnect");
          }
        }
        break;
      }
      case JobKind::Effect: {
        if (job.effect_op) {
          const auto r = job.effect_op(impl_->bus.Effect());
          LogResult(push, cardproto::Target::Effect, r);
          if (r.status == cardproto::Status::Timeout) {
            halted_.store(true);
            push("*** bus fault — Soft Recover or reconnect");
          }
        }
        break;
      }
      case JobKind::Gain: {
        const auto r = impl_->bus.Channel().SetGain(1, job.atten_db);
        if (r.status == cardproto::Status::Ok) {
          push("ok: g 1 " + std::to_string(job.atten_db));
        } else {
          push("err: gain");
        }
        break;
      }
      case JobKind::Silence: {
        const auto r = impl_->bus.Channel().AllNotesOff();
        {
          std::lock_guard<std::mutex> lock(impl_->mu_);
          impl_->sent_hz_.fill(0.0);
          impl_->desired_hz_.fill(0.0);
          impl_->ClearWatchLocked();
        }
        push(r.status == cardproto::Status::Ok ? "ok: silence"
                                                  : "err: silence");
        break;
      }
      case JobKind::Recover: {
        const bool ok = impl_->bus.SoftRecover();
        if (!ok) {
          impl_->bus.ForceClearBus();
        }
        {
          std::lock_guard<std::mutex> lock(impl_->mu_);
          impl_->desired_hz_.fill(0.0);
          impl_->sent_hz_.fill(0.0);
          impl_->ClearWatchLocked();
        }
        halted_.store(false);
        impl_->bus.ClearBusFault();
        push(ok ? "ok: soft recover" : "ok: force clear bus");
        break;
      }
      }
      continue;
    }

    if (have_note) {
      cardproto::Result r;
      if (note_hz <= 0.0) {
        r = impl_->bus.Channel().NoteOff(note_slot);
      } else {
        r = impl_->bus.Channel().SetNote(note_slot, note_hz);
      }

      if (r.status == cardproto::Status::Ok) {
        std::lock_guard<std::mutex> lock(impl_->mu_);
        impl_->MarkNoteSentLocked(note_slot, note_hz);
      } else {
        halted_.store(true);
        push("*** RS485 fault on n" +
             std::string(1, "0123456789abcdef"[note_slot]) +
             " — note TX stopped");
        if (!impl_->bus.SoftRecover()) {
          impl_->bus.ForceClearBus();
        }
        std::lock_guard<std::mutex> lock(impl_->mu_);
        impl_->desired_hz_.fill(0.0);
        impl_->sent_hz_.fill(0.0);
        impl_->ClearWatchLocked();
      }
      continue;
    }

    if (do_vq) {
      const cardproto::Result r = impl_->bus.Channel().QueryVoiceStatus();
      if (r.ok()) {
        {
          std::lock_guard<std::mutex> lock(impl_->mu_);
          impl_->vq_misses_ = 0;
        }
        handle_vq(r);
      } else if (r.status == cardproto::Status::Timeout ||
                 r.status == cardproto::Status::IoError) {
        /* Status poll only — a late USB/ISO stall must not halt notes. */
        int misses = 0;
        bool give_up = false;
        IdleHandler handler;
        std::array<bool, kVqVoices> become_idle{};
        {
          std::lock_guard<std::mutex> lock(impl_->mu_);
          ++impl_->vq_misses_;
          misses = impl_->vq_misses_;
          give_up = misses >= kVqMissLimit;
          if (give_up) {
            handler = impl_->idle_handler_;
            for (uint8_t i = 0; i < kVqVoices; ++i) {
              if (impl_->watch_idle_[i]) {
                impl_->watch_idle_[i] = false;
                become_idle[i] = true;
              }
            }
            impl_->vq_misses_ = 0;
          }
        }
        push_poll("warn: vq timeout (" + std::to_string(misses) + "/" +
                  std::to_string(kVqMissLimit) + ")");
        if (give_up) {
          push_poll("warn: vq stalled — stopped UAC watch");
          if (handler) {
            for (uint8_t i = 0; i < kVqVoices; ++i) {
              if (become_idle[i]) {
                handler(i);
              }
            }
          }
        }
      } else {
        push_poll(std::string("warn: vq failed — ") +
                  (r.raw[0] ? r.raw : "bad reply"));
      }
    }
  }
}
