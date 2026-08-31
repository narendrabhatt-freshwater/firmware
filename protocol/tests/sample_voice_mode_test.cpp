#include "cardlink/audio/sample_dry.hpp"
#include "cardlink/midi/voice_bank.hpp"
#include "cardlink/usb/stream_proto.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
void Check(bool ok,const char *message){if(!ok){std::cerr<<message<<'\n';std::exit(EXIT_FAILURE);}}
void Load(cardlink::audio::SampleDryMixer &m,uint16_t wave){std::vector<int16_t>b(2048,123);std::string e;Check(m.SetBody(wave,b.data(),b.size(),e),"load body");}
cardproto::VoiceQuery Pending(uint8_t voice,uint8_t session){cardproto::VoiceQuery q;q.pending_mask=(uint8_t)(1u<<voice);q.best=voice;q.capacity=12240u;q.target_session[voice]=session;q.free_samples[voice]=12240u;return q;}
}

int main()
{
  cardlink::midi::VoiceBank bank;
  Check(bank.SetVoiceLimit(1u).empty(),"mono limit");
  const auto first=bank.NoteOn(84u);
  Check(first.size()==1u&&first[0].slot==0u,"mono first note");
  const auto steal=bank.NoteOn(60u);
  Check(steal.size()==2u&&steal[0].kind==cardlink::midi::BankEventKind::Steal&&
            steal[1].slot==0u,"mono replacement must reuse slot");

  cardlink::audio::SampleDryMixer mixer;
  Load(mixer,84u);Load(mixer,60u);
  constexpr unsigned frame=cardlink::usb::kStreamUacBodySamples;
  std::array<int16_t,frame> packet{};bool sof=false;uint8_t session=0u;
  mixer.NoteOnSession(0u,84u,1u);mixer.DrainPendingCommands();
  mixer.ApplyVoiceStatus(Pending(0u,1u));
  Check(mixer.FillUacFrame(0u,packet.data(),packet.size(),sof,session)==frame&&sof,
        "first mono generation needs SOF");
  mixer.NoteOnSession(0u,60u,2u);mixer.DrainPendingCommands();
  Check(!mixer.BurstIsCurrent(0u,1u,84u),"steal must stale old host session");
  mixer.ApplyVoiceStatus(Pending(0u,2u));
  Check(mixer.FillUacFrame(0u,packet.data(),packet.size(),sof,session)==frame&&
            sof&&session==2u,
        "replacement must stream only its new session");
  mixer.Silence(0u);mixer.DrainPendingCommands();
  Check(mixer.HasBody(60u),"host voice reconciliation must preserve loaded BODY");
  Check(mixer.NoteOnSession(0u,60u,3u)==3u,
        "preserved BODY must be immediately reusable after reconciliation");
  mixer.DrainPendingCommands();

  cardlink::midi::VoiceBank duo;
  Check(duo.SetVoiceLimit(2u).empty(),"duo limit");
  const auto a=duo.NoteOn(60u);const auto b=duo.NoteOn(69u);
  Check(a.back().slot!=b.back().slot&&duo.ActiveCount()==2u,
        "polyphonic notes must use independent physical streams");

  cardlink::audio::SampleDryMixer startup;
  Load(startup,0u);
  startup.NoteOnSession(0u,0u,1u,0.0,440.0);
  startup.NoteOnSession(1u,0u,1u,0.0,440.0);
  startup.DrainPendingCommands();
  cardproto::VoiceQuery both;
  both.pending_mask=0x03u;both.capacity=12240u;both.best=0u;
  for(uint8_t v=0u;v<2u;++v){both.target_session[v]=1u;both.free_samples[v]=12240u;}
  startup.ApplyVoiceStatus(both);
  const uint8_t first_voice=startup.HungriestUacWant(frame);
  Check(first_voice<2u,"startup first voice");
  Check(startup.FillUacFrame(first_voice,packet.data(),packet.size(),sof,session)==frame,
        "startup first frame");
  Check(startup.HungriestUacWant(frame)!=first_voice,
        "a silent voice may start when its exact 1 ms service cost fits before the playing deadline");

  /* Deterministic full-capacity transport model: eight voices consume exactly
   * 500 source samples/ms in aggregate, vq arrives every 5 ms, and UAC can
   * route one 508-sample frame/ms. Pending voices consume immediately after
   * their first accepted frame, which is stricter than the real 2 ms script. */
  cardlink::audio::SampleDryMixer capacity;
  Load(capacity,0u);
  capacity.SetBodyRootHz(0u,48.0);
  std::array<unsigned,cardlink::audio::kSampleVoices> card_fill{};
  std::array<bool,cardlink::audio::kSampleVoices> card_started{};
  std::array<unsigned,cardlink::audio::kSampleVoices> consume_fraction{};
  uint16_t card_ack=0u;
  unsigned holds=0u;
  for(uint8_t v=0u;v<cardlink::audio::kSampleVoices;++v)
    capacity.NoteOnSession(v,0u,1u,0.0,62.5);
  capacity.DrainPendingCommands();
  for(unsigned ms=0u;ms<20000u;++ms){
    for(uint8_t v=0u;v<cardlink::audio::kSampleVoices;++v){
      if(!card_started[v])continue;
      consume_fraction[v]+=625u;
      const unsigned consume=consume_fraction[v]/10u;
      consume_fraction[v]%=10u;
      if(card_fill[v]<consume){++holds;card_fill[v]=0u;}
      else card_fill[v]-=consume;
    }
    if(ms%5u==0u){
      cardproto::VoiceQuery q;
      q.capacity=12240u;q.uac_sequence=card_ack;q.best=0xFFu;
      for(uint8_t v=0u;v<cardlink::audio::kSampleVoices;++v){
        const uint8_t bit=static_cast<uint8_t>(1u<<v);
        if(card_started[v])q.active_mask=static_cast<uint8_t>(q.active_mask|bit);
        else q.pending_mask=static_cast<uint8_t>(q.pending_mask|bit);
        q.target_session[v]=1u;
        q.target_fill[v]=static_cast<uint16_t>(card_fill[v]);
        q.free_samples[v]=static_cast<uint16_t>(q.capacity-card_fill[v]);
      }
      capacity.ApplyVoiceStatus(q);
    }
    capacity.ConsumeOutputSamples(48.0);
    const uint8_t voice=capacity.HungriestUacWant(frame);
    if(voice<cardlink::audio::kSampleVoices){
      Check(card_fill[voice]+frame<=12240u,"exact credit must prevent ring overfill");
      Check(capacity.FillUacFrame(voice,packet.data(),packet.size(),sof,session)==frame,
            "credited capacity frame");
      card_ack=capacity.RecordUacSubmission(voice,frame);
      card_fill[voice]+=frame;card_started[voice]=true;
    }
  }
  Check(holds==0u,"exact 500 samples/ms must sustain 12,240-sample rings without holds");
  for(bool started:card_started)Check(started,"capacity scheduler must admit every voice");
  return EXIT_SUCCESS;
}
