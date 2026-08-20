#include "cardlink/rs485/controller.hpp"

#include "cardlink/rs485/bus.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>

namespace cardlink::rs485
{
namespace
{

constexpr uint8_t kVqVoices = 8;
/* Sequential vq ping-pong is already spaced by the adapter RTT (~1.2 ms).
 * Do not wait extra while a watch is live. A non-zero idle wait is only
 * so the worker does not spin when no notes or jobs are pending. */
constexpr auto kWorkerIdleWait = std::chrono::milliseconds(10);
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

void LogResult(const Controller::LogHandler &push,
               cardproto::Target target,
               const cardproto::Result &result)
{
  const char *tag = (target == cardproto::Target::Effect)    ? "[E] "
                    : (target == cardproto::Target::Channel) ? "[C] "
                                                             : "[*] ";
  if (result.status == cardproto::Status::Ok) {
    push(std::string(tag) + (result.raw[0] ? result.raw : "ok"));
  } else {
    push(std::string(tag) + "err: " +
         (result.raw[0] ? result.raw : "timeout/io"));
  }
}

bool ParseVq(const char *raw, uint8_t &mask_out, uint8_t &best_out,
             std::array<uint8_t, kVqVoices> &slots_out)
{
  cardproto::VoiceQuery query;
  if (!cardproto::ParseVoiceQuery(raw, query)) {
    return false;
  }
  mask_out = query.mask;
  best_out = query.best;
  slots_out = query.free_slots;
  return true;
}

} // namespace

struct Controller::Impl
{
  enum class JobKind : uint8_t
  {
    Notes = 0,
    Exec,
    Gain,
    Silence,
    Recover,
    Channel,
    Effect,
  };

  struct Job
  {
    JobKind kind = JobKind::Notes;
    cardproto::Target target = cardproto::Target::Channel;
    std::string command;
    uint8_t atten_db = 0;
    ChannelOp channel_op;
    EffectOp effect_op;
  };

  Bus bus;
  mutable std::mutex mutex;
  std::condition_variable cv;
  std::array<double, cardlink::midi::kVoiceCount> desired_hz{};
  std::array<double, cardlink::midi::kVoiceCount> sent_hz{};
  std::array<bool, kVqVoices> watch_idle{};
  std::array<bool, kVqVoices> underrun_logged{};
  int vq_misses = 0;
  std::queue<Job> jobs;
  LogHandler log_handler;
  LogHandler poll_log_handler;
  CommandHandler command_handler;
  IdleHandler idle_handler;
  VqHandler vq_handler;
  std::atomic<bool> run{false};
  std::atomic<bool> open{false};
  std::atomic<bool> connecting{false};
  std::atomic<bool> halted{false};
  std::atomic<uint32_t> atten_db{6};
  std::thread worker;
  std::thread open_thread;

  bool WorkPendingLocked() const
  {
    for (uint8_t i = 0; i < cardlink::midi::kVoiceCount; ++i) {
      if (!HzEqual(desired_hz[i], sent_hz[i])) {
        return true;
      }
    }
    return false;
  }

  bool WatchPendingLocked() const
  {
    for (bool watching : watch_idle) {
      if (watching) {
        return true;
      }
    }
    return false;
  }

  void MarkNoteSentLocked(uint8_t slot, double hz)
  {
    sent_hz[slot] = hz;
    if (slot < kVqVoices) {
      /* Note-on and note-off both watch until vq reports card-side idle. */
      watch_idle[slot] = true;
      underrun_logged[slot] = false;
    }
  }

  void ClearWatchLocked()
  {
    watch_idle.fill(false);
    underrun_logged.fill(false);
    vq_misses = 0;
  }

  void Log(const std::string &message)
  {
    LogHandler handler;
    {
      std::lock_guard<std::mutex> lock(mutex);
      handler = log_handler;
    }
    if (handler) {
      handler(message);
    }
  }

  void LogPoll(const std::string &message)
  {
    LogHandler handler;
    {
      std::lock_guard<std::mutex> lock(mutex);
      handler = poll_log_handler ? poll_log_handler : log_handler;
    }
    if (handler) {
      handler(message);
    }
  }

  QueueResult Enqueue(Job job)
  {
    if (connecting.load()) {
      return QueueResult::Connecting;
    }
    if (!open.load()) {
      return QueueResult::Closed;
    }
    if (halted.load()) {
      return QueueResult::Halted;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      jobs.push(std::move(job));
    }
    cv.notify_one();
    return QueueResult::Ok;
  }

  void HandleVq(const cardproto::Result &result)
  {
    uint8_t mask = 0;
    uint8_t best = 0xFF;
    std::array<uint8_t, kVqVoices> slots{};
    if (!ParseVq(result.raw, mask, best, slots)) {
      LogPoll(std::string("warn: bad vq reply: ") +
              (result.raw[0] ? result.raw : "(empty)"));
      return;
    }

    IdleHandler idle;
    VqHandler vq;
    std::array<bool, kVqVoices> become_idle{};
    {
      std::lock_guard<std::mutex> lock(mutex);
      idle = idle_handler;
      vq = vq_handler;
      for (uint8_t i = 0; i < kVqVoices; ++i) {
        const bool active = (mask & static_cast<uint8_t>(1u << i)) != 0;
        /* Slot code 15 is reserved for a genuinely empty body ring. */
        if (active && slots[i] == 15u) {
          if (!underrun_logged[i]) {
            LogHandler handler =
                poll_log_handler ? poll_log_handler : log_handler;
            if (handler) {
              handler("warn: ring underrun voice " + std::to_string(i));
            }
            underrun_logged[i] = true;
          }
        } else {
          underrun_logged[i] = false;
        }
        /* BODY NoteOn is posted before nX is on the wire. An in-flight
         * vq then has no mask bit for the new slot; treating that as
         * idle Silence()s the body stream and only the AXI attack
         * plays. Stay armed while the host still wants a pitch. */
        if (watch_idle[i] && !active && desired_hz[i] <= 0.0) {
          watch_idle[i] = false;
          become_idle[i] = true;
        }
      }
    }

    if (vq) {
      vq(mask, best, slots);
    }
    if (!idle) {
      return;
    }
    for (uint8_t i = 0; i < kVqVoices; ++i) {
      if (become_idle[i]) {
        LogPoll("ok: voice " + std::to_string(i) + " idle → silence BODY");
        idle(i);
      }
    }
  }

  void HandleVqFailure(const cardproto::Result &result)
  {
    if (result.status != cardproto::Status::Timeout &&
        result.status != cardproto::Status::IoError) {
      LogPoll(std::string("warn: vq failed — ") +
              (result.raw[0] ? result.raw : "bad reply"));
      return;
    }

    /* Status poll only: a late USB stall must not halt notes. */
    int misses = 0;
    bool give_up = false;
    IdleHandler idle;
    std::array<bool, kVqVoices> become_idle{};
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++vq_misses;
      misses = vq_misses;
      give_up = misses >= kVqMissLimit;
      if (give_up) {
        idle = idle_handler;
        for (uint8_t i = 0; i < kVqVoices; ++i) {
          if (watch_idle[i]) {
            watch_idle[i] = false;
            become_idle[i] = true;
          }
        }
        vq_misses = 0;
      }
    }
    LogPoll("warn: vq timeout (" + std::to_string(misses) + "/" +
            std::to_string(kVqMissLimit) + ")");
    if (!give_up) {
      return;
    }
    LogPoll("warn: vq stalled — stopped BODY watch");
    if (idle) {
      for (uint8_t i = 0; i < kVqVoices; ++i) {
        if (become_idle[i]) {
          idle(i);
        }
      }
    }
  }

  void MarkFault(const std::string &message)
  {
    halted.store(true);
    Log(message);
  }

  void HandleJob(Job &job)
  {
    switch (job.kind) {
    case JobKind::Notes:
      break;
    case JobKind::Exec: {
      const auto result = bus.Exec(job.target, job.command);
      LogResult([this](const std::string &s) { Log(s); }, job.target, result);
      if (result.ok()) {
        const char *text = job.command.c_str();
        if (job.command.size() >= 2 &&
            (text[0] == 'c' || text[0] == '*') && text[1] == ':') {
          text += 2;
        }
        unsigned channel = 0;
        unsigned gain = 0;
        if (job.target != cardproto::Target::Effect &&
            std::sscanf(text, "g %u %u", &channel, &gain) == 2 &&
            channel == 1u && gain <= 127u) {
          atten_db.store(gain);
        }
        CommandHandler handler;
        {
          std::lock_guard<std::mutex> lock(mutex);
          handler = command_handler;
        }
        if (handler) {
          handler(job.target, job.command, result);
        }
      }
      if (result.status == cardproto::Status::Timeout) {
        MarkFault("*** bus fault — Soft Recover or reconnect");
      }
      break;
    }
    case JobKind::Channel:
      if (job.channel_op) {
        const auto result = job.channel_op(bus.Channel());
        LogResult([this](const std::string &s) { Log(s); },
                  cardproto::Target::Channel, result);
        if (result.status == cardproto::Status::Timeout) {
          MarkFault("*** bus fault — Soft Recover or reconnect");
        }
      }
      break;
    case JobKind::Effect:
      if (job.effect_op) {
        const auto result = job.effect_op(bus.Effect());
        LogResult([this](const std::string &s) { Log(s); },
                  cardproto::Target::Effect, result);
        if (result.status == cardproto::Status::Timeout) {
          MarkFault("*** bus fault — Soft Recover or reconnect");
        }
      }
      break;
    case JobKind::Gain: {
      const auto result = bus.Channel().SetGain(1, job.atten_db);
      Log(result.ok() ? "ok: g 1 " + std::to_string(job.atten_db)
                      : "err: gain");
      break;
    }
    case JobKind::Silence: {
      const auto result = bus.Channel().AllNotesOff();
      {
        std::lock_guard<std::mutex> lock(mutex);
        sent_hz.fill(0.0);
        desired_hz.fill(0.0);
        ClearWatchLocked();
      }
      Log(result.ok() ? "ok: silence" : "err: silence");
      break;
    }
    case JobKind::Recover: {
      const bool recovered = bus.SoftRecover();
      if (!recovered) {
        bus.ForceClearBus();
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        desired_hz.fill(0.0);
        sent_hz.fill(0.0);
        ClearWatchLocked();
      }
      halted.store(false);
      bus.ClearBusFault();
      Log(recovered ? "ok: soft recover" : "ok: force clear bus");
      break;
    }
    }
  }

  void WorkerMain()
  {
    while (true) {
      Job job;
      bool have_job = false;
      uint8_t note_slot = 0;
      double note_hz = 0.0;
      bool have_note = false;
      bool do_vq = false;

      {
        std::unique_lock<std::mutex> lock(mutex);
        const auto wait =
            (!halted.load() && WatchPendingLocked())
                ? std::chrono::milliseconds(0)
                : kWorkerIdleWait;
        cv.wait_for(lock, wait, [&] {
          return !run.load() || !jobs.empty() ||
                 (!halted.load() && WorkPendingLocked());
        });
        if (!run.load()) {
          break;
        }
        if (!jobs.empty()) {
          job = std::move(jobs.front());
          jobs.pop();
          have_job = true;
        } else if (!halted.load()) {
          for (uint8_t i = 0; i < cardlink::midi::kVoiceCount; ++i) {
            if (desired_hz[i] <= 0.0 && sent_hz[i] > 0.0) {
              note_slot = i;
              note_hz = 0.0;
              have_note = true;
              break;
            }
          }
          if (!have_note) {
            for (uint8_t i = 0; i < cardlink::midi::kVoiceCount; ++i) {
              if (!HzEqual(desired_hz[i], sent_hz[i])) {
                note_slot = i;
                note_hz = desired_hz[i];
                have_note = true;
                break;
              }
            }
          }
          if (!have_note && WatchPendingLocked()) {
            do_vq = true;
          }
        }
      }

      if (have_job) {
        HandleJob(job);
        continue;
      }
      if (have_note) {
        const auto result = note_hz <= 0.0
                                ? bus.Channel().NoteOff(note_slot)
                                : bus.Channel().SetNote(note_slot, note_hz);
        if (result.ok()) {
          std::lock_guard<std::mutex> lock(mutex);
          MarkNoteSentLocked(note_slot, note_hz);
        } else {
          MarkFault("*** RS485 fault on n" +
                    std::string(1, "0123456789abcdef"[note_slot]) +
                    " — note TX stopped");
          if (!bus.SoftRecover()) {
            bus.ForceClearBus();
          }
          std::lock_guard<std::mutex> lock(mutex);
          desired_hz.fill(0.0);
          sent_hz.fill(0.0);
          ClearWatchLocked();
        }
        continue;
      }
      if (do_vq) {
        const auto result = bus.Channel().QueryVoiceStatus();
        if (result.ok()) {
          {
            std::lock_guard<std::mutex> lock(mutex);
            vq_misses = 0;
          }
          HandleVq(result);
        } else {
          HandleVqFailure(result);
        }
      }
    }
  }
};

Controller::Controller() : impl_(std::make_unique<Impl>()) {}

Controller::~Controller()
{
  if (!impl_) {
    return;
  }
  if (impl_->open_thread.joinable()) {
    impl_->open_thread.join();
  }
  if (impl_->open.load()) {
    Close();
  }
}

bool Controller::Open(const std::string &path, uint32_t baud,
                      uint32_t atten_db)
{
  if (impl_->open.load()) {
    Close();
  }

  BusOptions options;
  options.baud = baud;
  options.atten_db = atten_db;
  options.effect_echo = EffectEcho::Off;
  options.allow_missing_effect = true;
  options.idle_gap_ms = 0;
  options.post_ack_settle_ms = 0;
  options.reply_timeout_ms = 400;
  options.retries = 1;

  const auto result = impl_->bus.Open(path, options);
  if (!result.ok()) {
    impl_->Log(std::string("err: open ") + path + " — " +
               (result.raw[0] ? result.raw : "failed"));
    return false;
  }

  impl_->atten_db.store(atten_db);
  impl_->halted.store(false);
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->desired_hz.fill(0.0);
    impl_->sent_hz.fill(0.0);
    impl_->ClearWatchLocked();
    while (!impl_->jobs.empty()) {
      impl_->jobs.pop();
    }
  }
  impl_->run.store(true);
  impl_->worker = std::thread([this] { impl_->WorkerMain(); });
  impl_->open.store(true);
  impl_->Log("ok: connected " + path + " gain " +
             std::to_string(atten_db) + " dB");
  return true;
}

void Controller::RequestOpen(const std::string &path, uint32_t baud,
                             uint32_t atten_db)
{
  if (impl_->connecting.exchange(true)) {
    impl_->Log("err: bus connect already in progress");
    return;
  }
  if (impl_->open_thread.joinable()) {
    impl_->open_thread.join();
  }
  impl_->open_thread = std::thread([this, path, baud, atten_db] {
    (void)Open(path, baud, atten_db);
    impl_->connecting.store(false);
  });
}

void Controller::Close()
{
  if (impl_->open_thread.joinable() &&
      impl_->open_thread.get_id() != std::this_thread::get_id()) {
    impl_->open_thread.join();
  }
  impl_->connecting.store(false);
  if (!impl_->open.exchange(false)) {
    return;
  }
  impl_->run.store(false);
  impl_->cv.notify_all();
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  impl_->bus.Close();
  impl_->halted.store(false);
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->ClearWatchLocked();
  }
  impl_->Log("ok: disconnected");
}

bool Controller::IsOpen() const { return impl_->open.load(); }
bool Controller::IsConnecting() const { return impl_->connecting.load(); }
bool Controller::BusFault() const { return impl_->halted.load(); }
std::string Controller::Path() const { return impl_->bus.Path(); }
uint32_t Controller::AttenDb() const { return impl_->atten_db.load(); }
uint32_t Controller::TimeoutCount() const
{
  return impl_->bus.TimeoutCount();
}
uint32_t Controller::ErrCount() const { return impl_->bus.ErrCount(); }

std::size_t Controller::QueueDepth() const
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->jobs.size();
}

void Controller::PublishBank(const cardlink::midi::VoiceBank &bank)
{
  if (!impl_->open.load() || impl_->halted.load()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto &slots = bank.Slots();
    for (uint8_t i = 0; i < cardlink::midi::kVoiceCount; ++i) {
      impl_->desired_hz[i] =
          slots[i].active && slots[i].freq_hz > 0.0
              ? ClampNoteHz(slots[i].freq_hz)
              : 0.0;
    }
    Impl::Job job;
    job.kind = Impl::JobKind::Notes;
    impl_->jobs.push(std::move(job));
  }
  impl_->cv.notify_one();
}

void Controller::RequestSilence()
{
  if (!impl_->open.load()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->desired_hz.fill(0.0);
    impl_->ClearWatchLocked();
    Impl::Job job;
    job.kind = Impl::JobKind::Silence;
    impl_->jobs.push(std::move(job));
  }
  impl_->cv.notify_one();
}

void Controller::RequestRecover()
{
  if (!impl_->open.load()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::Job job;
    job.kind = Impl::JobKind::Recover;
    impl_->jobs.push(std::move(job));
  }
  impl_->cv.notify_one();
}

QueueResult Controller::QueueExec(cardproto::Target target, std::string command)
{
  /* nX must last-win. Queueing every tap/release on RS485 delivers a stale
   * nX 0 after the hold's BODY session has already started. */
  if (target == cardproto::Target::Channel) {
    const char *text = command.c_str();
    if (command.size() >= 2 && text[0] == 'c' && text[1] == ':') {
      text += 2;
    }
    unsigned slot = 0;
    double hz = 0.0;
    if (text[0] == 'n' && text[1] != '\0' &&
        std::isdigit(static_cast<unsigned char>(text[1])) &&
        std::sscanf(text, "n%u %lf", &slot, &hz) == 2 &&
        slot < cardlink::midi::kVoiceCount) {
      if (impl_->connecting.load()) {
        return QueueResult::Connecting;
      }
      if (!impl_->open.load()) {
        return QueueResult::Closed;
      }
      if (impl_->halted.load()) {
        return QueueResult::Halted;
      }
      const double value = ClampNoteHz(hz);
      {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->desired_hz[slot] = value;
        /* Arm the idle watch after the card ACKs nX (MarkNoteSentLocked).
         * Arming here races an in-flight vq and drops the BODY stream. */
        impl_->underrun_logged[slot] = false;
      }
      impl_->cv.notify_one();
      return QueueResult::Ok;
    }
  }

  Impl::Job job;
  job.kind = Impl::JobKind::Exec;
  job.target = target;
  job.command = std::move(command);
  return impl_->Enqueue(std::move(job));
}

QueueResult Controller::QueueChannel(ChannelOp op)
{
  Impl::Job job;
  job.kind = Impl::JobKind::Channel;
  job.channel_op = std::move(op);
  return impl_->Enqueue(std::move(job));
}

QueueResult Controller::QueueEffect(EffectOp op)
{
  Impl::Job job;
  job.kind = Impl::JobKind::Effect;
  job.effect_op = std::move(op);
  return impl_->Enqueue(std::move(job));
}

QueueResult Controller::QueueGain(uint8_t atten_db)
{
  if (impl_->connecting.load()) {
    return QueueResult::Connecting;
  }
  if (!impl_->open.load()) {
    return QueueResult::Closed;
  }
  if (impl_->halted.load()) {
    return QueueResult::Halted;
  }
  impl_->atten_db.store(atten_db);
  Impl::Job job;
  job.kind = Impl::JobKind::Gain;
  job.atten_db = atten_db;
  return impl_->Enqueue(std::move(job));
}

void Controller::AcknowledgeSlotHz(uint8_t slot, double hz)
{
  if (slot >= cardlink::midi::kVoiceCount) {
    return;
  }
  const double value = ClampNoteHz(hz);
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->desired_hz[slot] = value;
  impl_->sent_hz[slot] = value;
  impl_->watch_idle[slot] = true;
  impl_->underrun_logged[slot] = false;
}

void Controller::AcknowledgeAllHz(double hz)
{
  const double value = ClampNoteHz(hz);
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->desired_hz.fill(value);
  impl_->sent_hz.fill(value);
  /* Every acknowledged voice watches until the card reports it idle. */
  for (uint8_t i = 0; i < kVqVoices; ++i) {
    impl_->watch_idle[i] = true;
    impl_->underrun_logged[i] = false;
  }
}

void Controller::SetLogHandler(LogHandler handler)
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->log_handler = std::move(handler);
}

void Controller::SetPollLogHandler(LogHandler handler)
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->poll_log_handler = std::move(handler);
}

void Controller::SetCommandHandler(CommandHandler handler)
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->command_handler = std::move(handler);
}

void Controller::SetIdleHandler(IdleHandler handler)
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->idle_handler = std::move(handler);
}

void Controller::SetVqHandler(VqHandler handler)
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->vq_handler = std::move(handler);
}

} // namespace cardlink::rs485
