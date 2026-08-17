#include "bus_controller.hpp"

#include <mutex>
#include <utility>

struct BusController::Impl
{
  std::mutex mutex;
  LogBuffer *log = nullptr;
  LogBuffer *poll_log = nullptr;
  std::vector<UiMirrorPatch> ui_mirror;
  /* Destroy first so worker callbacks cannot outlive the adapter state. */
  cardlink::rs485::Controller controller;

  void PushLog(const std::string &message)
  {
    LogBuffer *target = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex);
      target = log;
    }
    if (target) {
      target->Push(message);
    }
  }

  void PushPollLog(const std::string &message)
  {
    LogBuffer *target = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex);
      target = poll_log ? poll_log : log;
    }
    if (target) {
      target->Push(message);
    }
  }

  void HandleCommand(cardproto::Target target, const std::string &command,
                     const cardproto::Result &result)
  {
    UiMirrorPatch patch = ParseConsoleMirror(target, command, result);
    if (!patch.Any()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    ui_mirror.push_back(std::move(patch));
  }
};

BusController::BusController() : impl_(std::make_unique<Impl>())
{
  impl_->controller.SetLogHandler(
      [this](const std::string &message) { impl_->PushLog(message); });
  impl_->controller.SetPollLogHandler(
      [this](const std::string &message) { impl_->PushPollLog(message); });
  impl_->controller.SetCommandHandler(
      [this](cardproto::Target target, const std::string &command,
             const cardproto::Result &result) {
        impl_->HandleCommand(target, command, result);
      });
}

BusController::~BusController() = default;

bool BusController::Open(const std::string &path, uint32_t baud,
                         uint32_t atten_db, LogBuffer &log)
{
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->log = &log;
  }
  return impl_->controller.Open(path, baud, atten_db);
}

void BusController::RequestOpen(const std::string &path, uint32_t baud,
                                uint32_t atten_db, LogBuffer &log)
{
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->log = &log;
  }
  impl_->controller.RequestOpen(path, baud, atten_db);
}

void BusController::Close(LogBuffer &log)
{
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->log = &log;
  }
  impl_->controller.Close();
}

bool BusController::IsOpen() const { return impl_->controller.IsOpen(); }

bool BusController::IsConnecting() const
{
  return impl_->controller.IsConnecting();
}

bool BusController::BusFault() const
{
  return impl_->controller.BusFault();
}

std::string BusController::Path() const { return impl_->controller.Path(); }

void BusController::PublishBank(const cardlink::midi::VoiceBank &bank)
{
  impl_->controller.PublishBank(bank);
}

void BusController::RequestSilence()
{
  impl_->controller.RequestSilence();
}

void BusController::SetIdleHandler(IdleHandler handler)
{
  impl_->controller.SetIdleHandler(std::move(handler));
}

void BusController::SetVqHandler(VqHandler handler)
{
  impl_->controller.SetVqHandler(std::move(handler));
}

void BusController::RequestRecover(LogBuffer &log)
{
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->log = &log;
  }
  impl_->controller.RequestRecover();
  log.Push("… soft recover queued");
}

void BusController::SetPollLog(LogBuffer *poll_log)
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->poll_log = poll_log;
}

BusQueueResult BusController::QueueExec(cardproto::Target target,
                                        std::string command)
{
  return impl_->controller.QueueExec(target, std::move(command));
}

BusQueueResult BusController::QueueChannel(ChannelOp op)
{
  return impl_->controller.QueueChannel(std::move(op));
}

BusQueueResult BusController::QueueEffect(EffectOp op)
{
  return impl_->controller.QueueEffect(std::move(op));
}

BusQueueResult BusController::QueueGain(uint8_t atten_db)
{
  return impl_->controller.QueueGain(atten_db);
}

uint32_t BusController::AttenDb() const
{
  return impl_->controller.AttenDb();
}

uint32_t BusController::TimeoutCount() const
{
  return impl_->controller.TimeoutCount();
}

uint32_t BusController::ErrCount() const
{
  return impl_->controller.ErrCount();
}

std::size_t BusController::QueueDepth() const
{
  return impl_->controller.QueueDepth();
}

std::vector<UiMirrorPatch> BusController::DrainUiMirror()
{
  std::vector<UiMirrorPatch> patches;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  patches.swap(impl_->ui_mirror);
  return patches;
}

void BusController::AcknowledgeSlotHz(uint8_t slot, double hz)
{
  impl_->controller.AcknowledgeSlotHz(slot, hz);
}

void BusController::AcknowledgeAllHz(double hz)
{
  impl_->controller.AcknowledgeAllHz(hz);
}
