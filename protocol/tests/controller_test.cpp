#include "cardlink/rs485/controller.hpp"
#include "cardlink/rs485/types.hpp"

#include <cstdlib>
#include <cstring>
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

  uint8_t vq_frame[cardlink::rs485::kVqBinaryFrameLen] = {
      0xA5, 0x5A, 0x43, 0x01, 0xFF, 0x00,
      0xFF, 0xFF, 0xFF, 0xFF, 0x8B, 0x0A};
  const auto vq = cardlink::rs485::ParseVqBinaryReply(
      vq_frame, sizeof(vq_frame));
  Check(vq.ok() &&
            std::strcmp(vq.raw, "ok:vq ff 0 15 15 15 15 15 15 15 15") == 0,
        "binary vq reply parsing changed");
  vq_frame[10] ^= 0x01;
  Check(cardlink::rs485::ParseVqBinaryReply(vq_frame, sizeof(vq_frame)).status ==
            cardlink::rs485::Status::BadReply,
        "binary vq CRC corruption must be rejected");

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
