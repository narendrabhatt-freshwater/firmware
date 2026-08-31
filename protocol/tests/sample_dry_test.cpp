#include "cardlink/audio/sample_dry.hpp"
#include "cardlink/usb/stream_proto.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

using cardlink::audio::SampleDryMixer;
namespace {
void Check(bool ok,const char *message){if(!ok){std::cerr<<message<<'\n';std::exit(EXIT_FAILURE);}}
cardproto::VoiceQuery Status(uint8_t session,uint16_t capacity,uint16_t fill,uint16_t credit,bool pending=true,uint16_t uac_sequence=0u)
{
  cardproto::VoiceQuery q;
  q.active_mask=pending?0u:1u;q.pending_mask=pending?1u:0u;q.best=0u;
  q.capacity=capacity;q.status_sequence=1u;q.uac_sequence=uac_sequence;q.target_session[0]=session;
  q.target_fill[0]=fill;q.free_samples[0]=credit;
  return q;
}
}

int main()
{
  SampleDryMixer mixer;
  std::array<int16_t,2048> source{};
  constexpr unsigned frame=cardlink::usb::kStreamUacBodySamples;
  std::array<int16_t,frame> packet{};
  for(size_t i=0;i<source.size();++i)source[i]=static_cast<int16_t>(i);
  std::string error;
  Check(mixer.SetBody(0u,source.data(),source.size(),error),"load BODY");
  const uint8_t session=mixer.NoteOnSession(0u,0u,7u);
  mixer.DrainPendingCommands();
  Check(session==7u&&mixer.WantUacSamples(0u)==0u,
        "streaming must wait for versioned runtime credit");

  mixer.ApplyVoiceStatus(Status(6u,12240u,0u,12240u));
  Check(mixer.WantUacSamples(0u)==0u,"wrong target session must not grant credit");
  mixer.ApplyVoiceStatus(Status(7u,2000u,0u,frame));
  bool sof=false;uint8_t emitted_session=0u;
  Check(mixer.FillUacFrame(0u,packet.data(),packet.size(),sof,emitted_session)==frame&&
            sof&&emitted_session==7u,
        "first exact-credit frame must carry repeated SOF");
  const uint16_t first_sequence=mixer.RecordUacSubmission(0u,frame);
  Check(mixer.WantUacSamples(0u)==0u,"one status cannot be overspent");

  mixer.ApplyVoiceStatus(Status(7u,2000u,0u,frame,true,0u));
  Check(mixer.FillUacFrame(0u,packet.data(),packet.size(),sof,emitted_session)==0u,
        "an unacknowledged routed frame must consume exact reported credit");
  mixer.ApplyVoiceStatus(Status(7u,12240u,frame,12240u-frame,true,first_sequence));
  Check(mixer.FillUacFrame(0u,packet.data(),packet.size(),sof,emitted_session)==frame&&
            !sof,
        "confirmed initial BODY must switch to ordinary complete frames");
  mixer.ApplyVoiceStatus(Status(7u,2000u,2000u-(frame-1u),frame-1u,false,first_sequence));
  Check(mixer.FillUacFrame(0u,packet.data(),packet.size(),sof,emitted_session)==0u,
        "credit smaller than one BODY frame must never emit a partial frame");

  const uint8_t replacement=mixer.NoteOnSession(0u,0u,8u);
  mixer.DrainPendingCommands();
  Check(replacement==8u,"replacement session");
  mixer.ApplyVoiceStatus(Status(8u,12240u,0u,12240u));
  Check(mixer.FillUacFrame(0u,packet.data(),packet.size(),sof,emitted_session)==frame&&sof,
        "superseding note must receive its own SOF generation");
  return EXIT_SUCCESS;
}
