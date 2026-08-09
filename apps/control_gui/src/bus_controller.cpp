#include "bus_controller.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

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

void LogResult(const std::function<void(const std::string &)> &push,
               protocol::Target target,
               const protocol::Result &r)
{
  const char *tag = (target == protocol::Target::Effect)    ? "[E] "
                    : (target == protocol::Target::Channel) ? "[C] "
                                                         : "[*] ";
  if (r.status == protocol::Status::Ok) {
    push(std::string(tag) + (r.raw[0] ? r.raw : "ok"));
  } else {
    push(std::string(tag) + "err: " + (r.raw[0] ? r.raw : "timeout/io"));
  }
}

} // namespace

struct BusController::Impl
{
  host_io::rs485::Bus bus;

  std::mutex mu_;
  std::condition_variable cv_;
  std::array<double, midi_host::kVoiceCount> desired_hz_{};
  std::array<double, midi_host::kVoiceCount> sent_hz_{};
  std::queue<Job> jobs_;
  std::vector<UiMirrorPatch> ui_mirror_;
  std::atomic<bool> run_{false};
  std::thread worker_;
  std::thread open_thread_;
  LogBuffer *log_ = nullptr;
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

  host_io::rs485::BusOptions opts;
  opts.baud = baud;
  opts.atten_db = atten_db;
  opts.effect_echo = host_io::rs485::EffectEcho::Off;
  opts.allow_missing_effect = true;
  opts.idle_gap_ms = 0;
  opts.post_ack_settle_ms = 0;
  opts.reply_timeout_ms = 400;
  opts.retries = 1;

  const auto r = impl_->bus.Open(path, opts);
  if (r.status != protocol::Status::Ok) {
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

BusQueueResult BusController::QueueExec(protocol::Target target,
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

BusQueueResult BusController::QueueMode(protocol::PlayMode mode)
{
  return QueueChannel([mode](protocol::ChannelClient &ch) {
    return ch.SetMode(mode);
  });
}

BusQueueResult BusController::QueuePlayWave(uint8_t slot, double rate_hz)
{
  return QueueChannel([slot, rate_hz](protocol::ChannelClient &ch) {
    return ch.PlayWave(slot, rate_hz);
  });
}

BusQueueResult BusController::QueueStopWave(uint8_t slot)
{
  return QueueChannel([slot](protocol::ChannelClient &ch) {
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

  while (true) {
    Job job;
    bool have_job = false;
    uint8_t note_slot = 0;
    double note_hz = 0.0;
    bool have_note = false;

    {
      std::unique_lock<std::mutex> lock(impl_->mu_);
      impl_->cv_.wait(lock, [&] {
        if (!impl_->run_.load()) {
          return true;
        }
        if (!impl_->jobs_.empty()) {
          return true;
        }
        if (!halted_.load()) {
          for (uint8_t i = 0; i < midi_host::kVoiceCount; ++i) {
            if (!HzEqual(impl_->desired_hz_[i], impl_->sent_hz_[i])) {
              return true;
            }
          }
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
      }
    }

    if (have_job) {
      switch (job.kind) {
      case JobKind::Notes:
        break;
      case JobKind::Exec: {
        const auto r = impl_->bus.Exec(
            static_cast<protocol::Target>(job.target), job.command);
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
        if (r.status == protocol::Status::Timeout) {
          halted_.store(true);
          push("*** bus fault — Soft Recover or reconnect");
        }
        break;
      }
      case JobKind::Channel: {
        if (job.channel_op) {
          const auto r = job.channel_op(impl_->bus.Channel());
          LogResult(push, protocol::Target::Channel, r);
          if (r.status == protocol::Status::Timeout) {
            halted_.store(true);
            push("*** bus fault — Soft Recover or reconnect");
          }
        }
        break;
      }
      case JobKind::Effect: {
        if (job.effect_op) {
          const auto r = job.effect_op(impl_->bus.Effect());
          LogResult(push, protocol::Target::Effect, r);
          if (r.status == protocol::Status::Timeout) {
            halted_.store(true);
            push("*** bus fault — Soft Recover or reconnect");
          }
        }
        break;
      }
      case JobKind::Gain: {
        const auto r = impl_->bus.Channel().SetGain(1, job.atten_db);
        if (r.status == protocol::Status::Ok) {
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
        }
        push(r.status == protocol::Status::Ok ? "ok: silence"
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
        }
        halted_.store(false);
        impl_->bus.ClearBusFault();
        push(ok ? "ok: soft recover" : "ok: force clear bus");
        break;
      }
      }
      continue;
    }

    if (!have_note) {
      continue;
    }

    protocol::Result r;
    if (note_hz <= 0.0) {
      r = impl_->bus.Channel().NoteOff(note_slot);
    } else {
      r = impl_->bus.Channel().SetNote(note_slot, note_hz);
    }

    if (r.status == protocol::Status::Ok) {
      std::lock_guard<std::mutex> lock(impl_->mu_);
      impl_->sent_hz_[note_slot] = note_hz;
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
    }
  }
}
