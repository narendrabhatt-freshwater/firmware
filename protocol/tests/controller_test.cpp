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

  uint8_t vq_frame[cardlink::rs485::kVqBinaryFrameLen] = {};
  vq_frame[0]=0xA5;vq_frame[1]=0x5A;vq_frame[2]=0x43;vq_frame[3]=0x04;
  vq_frame[4]=0xFF;vq_frame[5]=0x01;vq_frame[6]=0;
  vq_frame[8]=0xD0;vq_frame[9]=0x2F;vq_frame[10]=0x34;vq_frame[11]=0x12;
  vq_frame[12]=0xCD;vq_frame[13]=0xAB;
  for(unsigned i=0;i<8u;++i){const unsigned at=14u+5u*i;vq_frame[at]=(uint8_t)i;vq_frame[at+3u]=0xD0;vq_frame[at+4u]=0x2F;}
  uint8_t crc=0u;for(unsigned i=0;i<54u;++i){crc^=vq_frame[i];for(unsigned bit=0;bit<8u;++bit)crc=(crc&0x80u)?(uint8_t)((crc<<1u)^0x07u):(uint8_t)(crc<<1u);}
  vq_frame[54]=crc;vq_frame[55]=0x0A;
  const auto vq = cardlink::rs485::ParseVqBinaryReply(
      vq_frame, sizeof(vq_frame));
  Check(vq.ok() &&
            std::strcmp(vq.raw,
                        "ok:vq7 ff 01 0 12240 4660 43981 0 0 12240 1 0 12240 2 0 12240 3 0 12240 4 0 12240 5 0 12240 6 0 12240 7 0 12240") == 0,
        "binary vq reply parsing changed");
  vq_frame[54] ^= 0x01;
  Check(cardlink::rs485::ParseVqBinaryReply(vq_frame, sizeof(vq_frame)).status ==
            cardlink::rs485::Status::BadReply,
        "binary vq CRC corruption must be rejected");

  controller.SetLogHandler([](const std::string &) {});
  controller.SetPollLogHandler([](const std::string &) {});
  controller.SetCommandHandler(
      [](cardproto::Target, const std::string &, const cardproto::Result &) {});
  controller.SetIdleHandler([](uint8_t) {});
  controller.SetVqHandler(
      [](const cardproto::VoiceQuery &) {});
  controller.AcknowledgeSlotKey(0, 69u);
  controller.AcknowledgeSlotKey(255, 69u);
  controller.AcknowledgeSlotOff(0);
  controller.RequestSilence();
  controller.RequestRecover();
  controller.Close();

  return EXIT_SUCCESS;
}
