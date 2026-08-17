#include "cardlink/rs485/controller.hpp"

#include <cstdlib>
#include <iostream>

namespace
{

void Check(bool condition, const char *message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

} // namespace

int main()
{
  using cardlink::rs485::Controller;
  using cardlink::rs485::QueueResult;

  Controller controller;
  Check(!controller.IsOpen(), "a new controller must be closed");
  Check(!controller.IsConnecting(), "a new controller must not be connecting");
  Check(!controller.BusFault(), "a new controller must not have a bus fault");
  Check(controller.Path().empty(), "a closed controller must have no path");
  Check(controller.QueueDepth() == 0, "a new controller queue must be empty");

  Check(controller.QueueExec(cardproto::Target::Channel, "s") ==
            QueueResult::Closed,
        "raw commands must reject while closed");
  Check(controller.QueueExec(cardproto::Target::Channel, "n0 440") ==
            QueueResult::Closed,
        "scheduled note commands must reject while closed");
  Check(controller.QueueChannel(
            [](cardproto::ChannelClient &) { return cardproto::Result{}; }) ==
            QueueResult::Closed,
        "typed Channel commands must reject while closed");
  Check(controller.QueueEffect(
            [](cardproto::EffectClient &) { return cardproto::Result{}; }) ==
            QueueResult::Closed,
        "typed Effect commands must reject while closed");
  Check(controller.QueueGain(6) == QueueResult::Closed,
        "gain commands must reject while closed");
  Check(controller.AttenDb() == 6 && controller.QueueDepth() == 0,
        "rejected commands must not mutate controller state");

  controller.SetLogHandler([](const std::string &) {});
  controller.SetPollLogHandler([](const std::string &) {});
  controller.SetCommandHandler(
      [](cardproto::Target, const std::string &, const cardproto::Result &) {});
  controller.SetIdleHandler([](uint8_t) {});
  controller.SetVqHandler(
      [](uint8_t, uint8_t, const std::array<uint8_t, 8> &) {});
  controller.AcknowledgeSlotHz(0, 440.0);
  controller.AcknowledgeSlotHz(255, 440.0);
  controller.AcknowledgeAllHz(0.0);
  controller.RequestSilence();
  controller.RequestRecover();
  controller.Close();

  return EXIT_SUCCESS;
}
