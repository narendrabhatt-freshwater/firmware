#include "note_bank.h"
#include "channel_vm.h"
#include "note_envelope.h"
#include "stream_ring.h"
#include "usb_stream.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int ok,const char *message){if(!ok){fprintf(stderr,"%s\n",message);exit(1);}}
static int8_t attack_tables[ATTACK_BANK_COUNT][ATTACK_BANK_LEN];
static uint32_t attack_lengths[ATTACK_BANK_COUNT];
static uint8_t attack_write_active;
static int32_t last_filter_sample;
static float led_red, led_green, led_blue, led_brightness;
float AttackBank_GetRootHz(uint16_t id){(void)id;return 260.0f;}
uint32_t AttackBank_GetLen(uint16_t id){return id<ATTACK_BANK_COUNT?attack_lengths[id]:0u;}
const int8_t *AttackBank_Table(uint16_t id){return id<ATTACK_BANK_COUNT?attack_tables[id]:NULL;}
void AttackBank_SetWriteActive(uint8_t active){attack_write_active=active;}
uint8_t AttackBank_WriteIsActive(void){return attack_write_active;}
void AttackBank_Stop(uint8_t note){(void)note;} void AttackBank_StopAll(void){}
int32_t NoteFilter_Process(uint8_t note,int32_t sample){(void)note;last_filter_sample=sample;return sample;}
void NoteFilter_Reset(uint8_t note){(void)note;} void NoteFilter_OnNoteFreq(uint8_t note,double hz){(void)note;(void)hz;}
int ChannelLed_Set(float red,float green,float blue,float brightness){led_red=red;led_green=green;led_blue=blue;led_brightness=brightness;return 0;}
static uint8_t *read_file(const char *path,size_t *size){FILE*f=fopen(path,"rb");long n;uint8_t*p;check(f!=NULL,"open FWSC");fseek(f,0,SEEK_END);n=ftell(f);rewind(f);p=malloc((size_t)n);check(p!=NULL&&fread(p,1,(size_t)n,f)==(size_t)n,"read FWSC");fclose(f);*size=(size_t)n;return p;}
static void boundary(void){NoteBank_VmBoundaryBegin();for(unsigned i=0;i<48u;++i)(void)NoteBank_NextSample();NoteBank_VmBoundaryEnd();}
static void boundaries(unsigned count){while(count--)boundary();}
static uint32_t render_peak(unsigned count){uint32_t peak=0u;NoteBank_VmBoundaryBegin();for(unsigned i=0;i<count;++i){int64_t s=NoteBank_NextSample();uint32_t a=(uint32_t)(s<0?-s:s);if(a>peak)peak=a;}NoteBank_VmBoundaryEnd();return peak;}
static void prime_body(uint8_t note){int8_t body[USB_STREAM_UAC_BODY_SAMPLES];for(unsigned i=0u;i<USB_STREAM_UAC_BODY_SAMPLES;++i)body[i]=(int8_t)((i&1u)!=0u?64:-64);check(StreamRing_WriteVoice(note,0xFFu,1u,NoteBank_GetWaveId(note),body,USB_STREAM_UAC_BODY_SAMPLES)==USB_STREAM_UAC_BODY_SAMPLES,"prime production BODY");}
static void prime_silent_body(uint8_t note){int8_t body[USB_STREAM_UAC_BODY_SAMPLES]={0};check(StreamRing_WriteVoice(note,0xFFu,1u,NoteBank_GetWaveId(note),body,USB_STREAM_UAC_BODY_SAMPLES)==USB_STREAM_UAC_BODY_SAMPLES,"prime silent BODY");}
static void hard_stop_case(const char *path)
{
  size_t size;
  uint8_t *program = read_file(path, &size);
  uint32_t dispatches[NOTE_BANK_VOICES];
  NoteBank_PanicAll();
  for (uint8_t v = 0u; v < NOTE_BANK_VOICES; ++v) {
    check(NoteBank_VmUploadBegin(v)==0 && NoteBank_VmUploadFeed(v,program,size)==0 &&
          NoteBank_VmUploadCommit(v)==0, "load hard-stop program");
  }
  for (uint8_t v = 0u; v < NOTE_BANK_VOICES; ++v) {
    check(NoteBank_NoteOn(v,60u,127u)==0,"start all eight voices");
    prime_body(v);
  }
  boundary();
  for (uint8_t v = 0u; v < NOTE_BANK_VOICES; ++v) {
    check(NoteBank_IsActive(v),"all voices sounding before hard stop");
    check(NoteBank_NoteOnSession(v,62u,127u,13u)==0,"queue tagged replacement");
    dispatches[v] = ChannelVm_Metrics(v)->dispatches;
  }
  check(led_brightness>0.0f,"script lights LED before hard stop");
  NoteBank_AllNotesOff();
  check(NoteBank_AnyActive(),"hard stop waits for audio boundary");
  check(NoteBank_NoteOn(0u,64u,127u)==-3,"new note waits for queued hard stop");
  check(render_peak(48u)==0u,"hard stop renders silence from first frame");
  check(led_red==0.0f && led_green==0.0f && led_blue==0.0f && led_brightness==0.0f,
        "hard stop clears latched LED output without script callbacks");
  check(!NoteBank_AnyBankReferences(),"hard stop clears active and queued notes");
  check(NoteBank_VmActiveMask()==0xffu,"hard stop retains all programs");
  for (uint8_t v = 0u; v < NOTE_BANK_VOICES; ++v) {
    int8_t body[USB_STREAM_UAC_BODY_SAMPLES]={0};
    check(NoteEnv_Amplitude(v)==0.0f,"hard stop clears envelope");
    check(ChannelVm_Metrics(v)->dispatches==dispatches[v],"hard stop invokes no Berry handlers");
    check(StreamRing_WriteVoice(v,13u,1u,NoteBank_GetWaveId(v),body,sizeof body)==0u,
          "late replacement BODY rejected");
  }
  NoteBank_AllNotesOff();
  check(render_peak(48u)==0u,"repeated hard stop remains silent");
  for (uint8_t v = 0u; v < NOTE_BANK_VOICES; ++v)
    check(NoteBank_NoteOff(v)==0,"late key release accepted harmlessly");
  check(render_peak(48u)==0u,"late key releases stay silent");
  for (uint8_t v = 0u; v < NOTE_BANK_VOICES; ++v)
    check(ChannelVm_Metrics(v)->dispatches==dispatches[v],"late release does not dirty Berry state");

  for (uint8_t v = 0u; v < NOTE_BANK_VOICES; ++v) {
    check(NoteBank_NoteOn(v,65u,127u)==0,"restart without script reload");
    prime_body(v);
  }
  boundary();
  for (uint8_t v = 0u; v < NOTE_BANK_VOICES; ++v) {
    check(NoteBank_IsActive(v) && !StreamRing_HasPending(v) && NoteEnv_Amplitude(v)>0.0f,
          "reset Berry state starts next note immediately");
  }
  check(led_brightness>0.0f,"next note can light LED again without reload");
  NoteBank_AllNotesOff();
  check(render_peak(48u)==0u,"hard stop clears sounding voices");
  /* Also cancel startup before the first BODY packet arrives. */
  check(NoteBank_NoteOnSession(0u,60u,127u,14u)==0,"queue startup without BODY");
  NoteBank_AllNotesOff();
  check(render_peak(48u)==0u && !NoteBank_AnyBankReferences(),"cancel unstarted note");
  {
    int8_t body[USB_STREAM_UAC_BODY_SAMPLES]={0};
    check(StreamRing_WriteVoice(0u,14u,1u,NoteBank_GetWaveId(0u),body,sizeof body)==0u,
          "late startup BODY rejected");
  }
  free(program);
}
static void envelope_limits_case(const char *path)
{
  size_t size;
  uint8_t *program = read_file(path, &size);
  NoteEnv_Init();StreamRing_Init();NoteBank_Init();
  for (uint8_t v = 0u; v < NOTE_BANK_VOICES; ++v)
    check(NoteBank_VmUploadBegin(v)==0 && NoteBank_VmUploadFeed(v,program,size)==0 &&
          NoteBank_VmUploadCommit(v)==0,"load envelope limits program");
  for (uint8_t key = 60u; key <= 63u; ++key) {
    for (uint8_t v = 0u; v < NOTE_BANK_VOICES; ++v) {
      check(NoteBank_NoteOn(v,key,127u)==0,"reuse voice with extreme envelope parameters");
      prime_body(v);
    }
    boundary();
    for (uint8_t v = 0u; v < NOTE_BANK_VOICES; ++v) {
      check(NoteBank_IsActive(v) && NoteEnv_Amplitude(v)==1.0f,"extreme attack reaches full amplitude");
      check(NoteBank_NoteOff(v)==0,"release with extreme slope");
    }
    boundary();
    for (uint8_t v = 0u; v < NOTE_BANK_VOICES; ++v)
      check(!NoteBank_IsActive(v) && NoteEnv_Amplitude(v)==0.0f &&
            NoteBank_VmIsActive(v) && NoteBank_VmFault(v)==FW_VM_FAULT_NONE,
            "extreme release ends note and retains program");
  }
  free(program);
}

int main(int argc,char **argv){
  uint8_t *program;size_t size;check(argc==6,"program paths required");program=read_file(argv[1],&size);
  envelope_limits_case(argv[5]);
  NoteEnv_Init();StreamRing_Init();NoteBank_Init();
  hard_stop_case(argv[2]);hard_stop_case(argv[4]);
  NoteEnv_Init();StreamRing_Init();NoteBank_Init();check(NoteBank_VmActiveMask()==0u,"reset has no programs");
  check(NoteBank_NoteOn(0u,60u,127u)==-2,"note reports no program");boundary();check(!NoteBank_IsActive(0u),"no-program silent");
  check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,program,size)==0&&NoteBank_VmUploadCommit(0u)==0,"valid FWSC activates");
  check(NoteBank_VmActiveMask()==1u,"only voice zero loaded");
  { uint16_t previous=NoteBank_GetWaveId(0u);
    check(NoteBank_NoteOnSampleSession(0u,248u,60u,100u,7u)!=0 && NoteBank_GetWaveId(0u)==previous,"combined command rejects reserved sample without assignment");
    check(NoteBank_NoteOnSampleSession(0u,3u,60u,100u,255u)!=0 && NoteBank_GetWaveId(0u)==previous,"failed combined note preserves sample assignment");
    check(NoteBank_NoteOnSampleSession(0u,3u,60u,100u,7u)==0 && NoteBank_GetWaveId(0u)==3u && StreamRing_TargetSession(0u)==7u,"combined command assigns sample and binds note session");
    NoteBank_PanicAll();check(NoteBank_SetWaveId(0u,previous)==0,"restore sample after combined command test");
  }
  AttackBank_SetWriteActive(1u);check(NoteBank_NoteOn(0u,60u,64u)==-3,"note rejected while attack-bank upload can tear tables");AttackBank_SetWriteActive(0u);
  check(NoteBank_NoteOn(0u,60u,64u)==0,"note accepted");prime_body(0u);boundary();
  check(NoteBank_GetKey(0u)==60u,"physical key applied");
  check(NoteBank_GetVelocity(0u)==64u,"note velocity applied");
  check(fabs(NoteBank_GetFreq(0u)-261.625565)<0.001,"C4 frequency resolved on card");
  check(NoteBank_IsActive(0u)&&NoteEnv_Amplitude(0u)>0.0f,"Berry starts native attack");
  check(StreamRing_FreeLevel(0u)<STREAM_RING_SAMPLES,"production BODY occupies ring credit");
  check(render_peak(256u)>0u,"signed-int8 production BODY renders");
  check(NoteBank_VmUploadBegin(1u)==-2,"reload rejected while sounding");
  check(NoteBank_NoteOff(0u)==0,"note off accepted");for(unsigned i=0;i<64u&&NoteBank_IsActive(0u);++i)boundary();
  check(!NoteBank_IsActive(0u),"release ends note");check(NoteBank_VmFaultCount(0u)==0u,"no integration fault");
  check(NoteBank_VmUploadBegin(0u)==0,"replacement begins idle");check(NoteBank_NoteOn(0u,60u,127u)==-3,"note-on rejected while uploading");NoteBank_VmUploadAbort(0u);
  check(NoteBank_VmIsActive(0u),"abort preserves program");
  {uint8_t bad[FW_SCRIPT_CONTAINER_HEADER_SIZE]={0};check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,bad,sizeof(bad))!=0,"bad FWSC rejected");check(NoteBank_VmIsActive(0u),"bad replacement preserved");}
  {uint8_t *bad_crc=malloc(size);check(bad_crc!=NULL,"allocate CRC case");memcpy(bad_crc,program,size);bad_crc[size-1u]^=1u;
   check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,bad_crc,size)==0&&NoteBank_VmUploadCommit(0u)!=0,"CRC failure rejected");
   check(NoteBank_VmIsActive(0u),"CRC failure preserves active program");free(bad_crc);}
  check(NoteBank_VmUploadBegin(1u)==0&&NoteBank_VmUploadFeed(1u,program,size)==0&&NoteBank_VmUploadCommit(1u)==0,"second voice load");
  check(NoteBank_VmActiveMask()==3u,"per-voice program mask");
  check(NoteBank_NoteOn(1u,69u,1u)==0,"second voice note accepted");prime_body(1u);boundary();
  check(NoteBank_GetKey(1u)==69u,"physical key remains unchanged");
  check(NoteBank_GetVelocity(1u)==1u,"minimum velocity survives note start");
  check(fabs(NoteBank_GetFreq(1u)-440.0)<0.001,"A4 uses standard pitch");
  check(NoteBank_NoteOn(0u,69u,127u)==0,"identity voice accepts same physical key");prime_body(0u);boundary();
  check(NoteBank_GetKey(0u)==69u&&fabs(NoteBank_GetFreq(0u)-440.0)<0.001,"voices use standard pitch independently");
  {uint8_t *abi7=malloc(size);int feed;check(abi7!=NULL,"allocate ABI7 case");memcpy(abi7,program,size);abi7[8]=7u;abi7[9]=0u;NoteBank_PanicAll();
   check(NoteBank_VmUploadBegin(0u)==0,"ABI7 upload begin");feed=NoteBank_VmUploadFeed(0u,abi7,size);
   check(feed!=0||NoteBank_VmUploadCommit(0u)!=0,"ABI7 container must be rejected");free(abi7);}
  {size_t example_size;uint8_t *example=read_file(argv[2],&example_size);NoteBank_VmUploadAbort(0u);
   for(unsigned i=248u;i<256u;++i){attack_lengths[i]=2u;attack_tables[i][0]=64;attack_tables[i][1]=64;}
   check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,example,example_size)==0&&NoteBank_VmUploadCommit(0u)==0,"load production example");
   check(NoteBank_NoteOn(0u,60u,40u)==0,"example first note");prime_body(0u);boundary();
   check(StreamRing_HasPending(0u)==0u,"example must start an idle voice immediately");
   check(NoteBank_NoteOn(0u,61u,80u)==0,"replacement may be staged");
   check(StreamRing_HasPending(0u)!=0u,"replacement ring is pending before dispatch");
   check(NoteBank_NoteOff(0u)==0,"note off accepted with pending replacement");boundary();
   check(StreamRing_HasPending(0u)==0u,"native note off must cancel pending replacement");
   for(unsigned i=0;i<64u&&NoteBank_IsActive(0u);++i)boundary();
   check(NoteBank_NoteOn(0u,62u,100u)==0,"example replacement note");prime_body(0u);boundary();
   check(StreamRing_HasPending(0u)==0u,"example must promote a replacement immediately");free(example);}
  NoteBank_PanicAll();
  {size_t channel_size;uint8_t *channel_program=read_file(argv[2],&channel_size);
   const unsigned heads[]={0u,1u,32u,512u};
   const uint8_t keys[]={48u,60u,72u};
   for(unsigned h=0;h<4u;++h)for(unsigned k=0;k<3u;++k){
    NoteBank_PanicAll();
    check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,channel_program,channel_size)==0&&NoteBank_VmUploadCommit(0u)==0,"load ATTACK-only test program");
    attack_lengths[0]=heads[h];memset(attack_tables[0],64,sizeof attack_tables[0]);
    check(NoteBank_NoteOn(0u,keys[k],127u)==0,"accept note without BODY");
    NoteBank_HoldCountClear();boundaries(30u);
    check(!NoteBank_IsActive(0u)&&StreamRing_HasPending(0u)&&NoteBank_HoldCount()==0u,"delayed USB leaves ATTACK untouched at every pitch and head length");
    int8_t body[USB_STREAM_UAC_BODY_SAMPLES]={0};
    check(StreamRing_WriteVoice(0u,0xFFu,1u,0u,body,499u)==499u,"accept first split BODY block");
    boundaries(10u);
    check(!NoteBank_IsActive(0u)&&StreamRing_PendingFill(0u)==499u,"partial startup data stays pending");
    check(StreamRing_WriteVoice(0u,0xFFu,1u,0u,body,498u)==498u,"accept all but last startup sample");
    boundary();check(!NoteBank_IsActive(0u),"997 startup samples stay pending");
    check(StreamRing_WriteVoice(0u,0xFFu,1u,0u,body,1u)==1u,"complete startup BODY");
    boundary();
    check(NoteBank_IsActive(0u)&&!StreamRing_HasPending(0u)&&NoteBank_HoldCount()==0u,"start on next boundary after 998 committed samples");
   }
   NoteBank_PanicAll();
   check(NoteBank_NoteOnSampleSession(0u,0u,72u,127u,11u)==0,"arm tagged startup");
   boundaries(8u);
   check(NoteBank_NoteOff(0u)==0,"cancel before first BODY");boundary();
   {int8_t body[USB_STREAM_UAC_BODY_SAMPLES]={0};
    check(StreamRing_WriteVoice(0u,11u,1u,0u,body,sizeof body)==0u,"late canceled session cannot start playback");}
   boundaries(8u);
   check(!NoteBank_IsActive(0u)&&!StreamRing_HasPending(0u),"canceled startup remains silent");
   attack_lengths[0]=0u;free(channel_program);}
  NoteBank_PanicAll();
  {size_t channel_size;uint8_t *channel_program=read_file(argv[2],&channel_size);
   check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,channel_program,channel_size)==0&&NoteBank_VmUploadCommit(0u)==0,"load channel for rapid replacement");
   check(NoteBank_NoteOn(0u,60u,127u)==0,"start rapid replacement test");prime_body(0u);boundary();boundaries(29u);
   check(NoteBank_NoteOn(0u,62u,100u)==0,"start replacement fade");prime_body(0u);boundary();
   check(NoteBank_NoteOn(0u,64u,100u)==0,"supersede replacement before its fade ends");
   boundaries(5u);
   check(NoteBank_VmIsActive(0u)&&NoteBank_VmFault(0u)==FW_VM_FAULT_NONE,"fade completion preserves the newest pending note without a VM fault");
   prime_body(0u);boundary();
   check(NoteBank_VmIsActive(0u)&&NoteBank_GetKey(0u)==64u&&StreamRing_HasPending(0u)==0u,"latest replacement starts after its BODY arrives");
   check(NoteBank_NoteOff(0u)==0,"release latest replacement");boundaries(50u);
   check(NoteBank_VmIsActive(0u)&&!NoteBank_IsActive(0u),"replacement can release without losing its program");
   NoteBank_PanicAll();
   check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,channel_program,channel_size)==0&&NoteBank_VmUploadCommit(0u)==0,"reload channel for replacement cancellation");
   check(NoteBank_NoteOn(0u,60u,127u)==0,"start cancellation test");prime_body(0u);boundary();boundaries(29u);
   check(NoteBank_NoteOn(0u,62u,100u)==0,"fade before cancellation");prime_body(0u);boundary();
   check(NoteBank_NoteOn(0u,64u,100u)==0,"supersede before cancellation");boundaries(5u);
   check(NoteBank_NoteOff(0u)==0,"release replacement before BODY arrives");boundaries(50u);
   check(NoteBank_VmIsActive(0u)&&NoteBank_VmFault(0u)==FW_VM_FAULT_NONE&&!NoteBank_IsActive(0u)&&!StreamRing_HasPending(0u),"note off must cancel deferred replacement without a stale start");
   check(NoteBank_NoteOn(0u,65u,100u)==0,"play after cancelled replacement");prime_body(0u);boundary();
   check(NoteBank_VmIsActive(0u)&&NoteBank_GetKey(0u)==65u,"program remains usable after cancellation");
   free(channel_program);}
  NoteBank_PanicAll();
  {size_t channel_size;uint8_t *channel_program=read_file(argv[2],&channel_size);
   check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,channel_program,channel_size)==0&&NoteBank_VmUploadCommit(0u)==0,"reload channel example");
   check(NoteBank_NoteOn(0u,60u,32u)==0,"low velocity note accepted");prime_body(0u);boundary();
   boundaries(49u);
   check(NoteBank_GetVelocity(0u)==32u,"low velocity reaches active voice");
   check(NoteEnv_Amplitude(0u)<32.0f/127.0f,"soft velocity attack remains below its peak at 50 ms");
   boundaries(150u);
   check(fabsf(NoteEnv_Amplitude(0u)-(32.0f/127.0f))<0.002f,"soft velocity attack reaches its peak near 200 ms");
   boundaries(500u);
   check(fabsf(NoteEnv_Amplitude(0u)-(32.0f/127.0f*0.9f))<0.002f,"decay reaches velocity-scaled sustain");
   check(NoteBank_NoteOff(0u)==0,"low velocity note off accepted");boundaries(50u);
   check(!NoteBank_IsActive(0u),"release retires low velocity note");
   check(NoteBank_NoteOn(0u,61u,127u)==0,"full velocity note accepted");prime_body(0u);boundary();boundaries(49u);
   check(fabsf(NoteEnv_Amplitude(0u)-1.0f)<0.0001f,"full velocity reaches full envelope amplitude");
   check(NoteBank_NoteOn(0u,62u,64u)==0,"replacement velocity accepted");prime_body(0u);boundary();
   check(StreamRing_HasPending(0u)!=0u,"replacement stays pending during steal fade");
   boundaries(4u);
   check(StreamRing_HasPending(0u)==0u,"replacement starts after 5 ms steal fade");
   boundaries(50u);
   check(NoteBank_GetVelocity(0u)==64u,"replacement keeps its own velocity");
   check(NoteEnv_Amplitude(0u)<64.0f/127.0f,"medium velocity attack remains below its peak at 50 ms");
   boundaries(50u);
   check(fabsf(NoteEnv_Amplitude(0u)-(64.0f/127.0f))<0.002f,"medium velocity attack reaches its peak near 100 ms");
   boundaries(500u);
   check(fabsf(NoteEnv_Amplitude(0u)-(64.0f/127.0f*0.9f))<0.002f,"replacement decay reaches sustain");
   NoteBank_PanicAll();
   check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,channel_program,channel_size)==0&&NoteBank_VmUploadCommit(0u)==0,"reload channel example for top key");
   check(NoteBank_NoteOn(0u,127u,127u)==0,"top MIDI key accepted");prime_body(0u);boundary();
   check(NoteBank_VmIsActive(0u)&&NoteBank_VmFault(0u)==FW_VM_FAULT_NONE,"transposed top key must stay within oscillator range");free(channel_program);}
  NoteBank_PanicAll();
  {size_t oscillator_size;uint8_t *oscillator_program=read_file(argv[3],&oscillator_size);
   for(unsigned i=248u;i<256u;++i){attack_lengths[i]=2u;attack_tables[i][0]=64;attack_tables[i][1]=64;}
   check(NoteBank_SetWaveId(0u,247u)==0&&NoteBank_SetWaveId(0u,248u)!=0,"last eight attack IDs must be reserved for oscillators");
   check(NoteBank_SetWaveId(0u,8u)==0,"sample may use a non-oscillator wave ID");
   check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,oscillator_program,oscillator_size)==0&&NoteBank_VmUploadCommit(0u)==0,"load oscillator program");
   check(NoteBank_NoteOn(0u,69u,127u)==0,"oscillator note accepted");prime_silent_body(0u);boundary();
   {uint32_t peak=render_peak(64u);check(NoteBank_IsActive(0u)&&peak>110000000u&&peak<125000000u,"sample and eight oscillators must be averaged before voice gain");
    check(last_filter_sample>900000000&&last_filter_sample<1000000000,"averaged source mix must feed the existing filter");}
   NoteBank_AllNotesOff();
   check(render_peak(48u)==0u && NoteBank_VmIsActive(0u),"hard stop silences oscillators and retains program");
   check(NoteBank_NoteOn(0u,69u,127u)==0,"oscillator restarts after hard stop");prime_silent_body(0u);boundary();
   check(render_peak(48u)>0u,"oscillator sounds again without reload");
   check(NoteBank_NoteOff(0u)==0,"oscillator note off accepted");boundary();check(!NoteBank_IsActive(0u),"oscillator note end clears the voice");free(oscillator_program);}
  attack_lengths[255]=0u;check(NoteBank_NoteOn(0u,69u,127u)==0,"invalid oscillator note posts");prime_silent_body(0u);boundary();
  check(!NoteBank_IsActive(0u)&&!NoteBank_VmIsActive(0u)&&NoteBank_VmFault(0u)==FW_VM_FAULT_HOST_CALL,"unloaded oscillator table must fault and silence only its voice");
  NoteBank_PanicAll();
  check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,program,size)==0&&NoteBank_VmUploadCommit(0u)==0,"restore simple program for depletion tests");
  check(NoteBank_SetWaveId(0u,0u)==0,"duration test wave");
  for(unsigned head=0u;head<=512u;head+=512u) {
    attack_lengths[0]=head;
    for(unsigned key=48u;key<=127u;++key) {
      NoteBank_PanicAll();
      check(NoteBank_NoteOn(0u,(uint8_t)key,127u)==0,"duration note accepted");
      prime_body(0u);boundary();
      if(head==0u) {
        double speed=NoteBank_GetFreq(0u)/260.0;
        while(speed>2.0) speed*=0.5;
        uint32_t inc=(uint32_t)(speed*65536.0+0.5);
        uint16_t demand=(uint16_t)(((uint64_t)inc*240u+65535u)>>16);
        check(NoteBank_RefillSamples5ms(0u)==demand,"card computes five-ms demand from its playback rate");
        check(demand<=480u,"folded notes stay within 2x BODY bandwidth");
        check(NoteBank_GetKey(0u)==key,"octave folding preserves the physical MIDI key");
      }
      uint32_t predicted=NoteBank_RemainingUs(0u), frames=0u;
      uint32_t last=predicted;
      if (head==0u && key==48u) {
        int8_t replacement[USB_STREAM_UAC_BODY_SAMPLES]={0};
        StreamRing_ArmPending(0u,0u,9u);
        check(NoteBank_RefillSamples5ms(0u)==0u,"pending session cannot advertise old voice consumption");
        check(StreamRing_WriteVoice(0u,9u,1u,0u,replacement,sizeof replacement)==sizeof replacement,"stage pending data beside current playback");
        check(NoteBank_RemainingUs(0u)==predicted,"pending samples must not extend current playback duration");
      }
      NoteBank_HoldCountClear();
      while(NoteBank_HoldCount()==0u && frames<10000u) {
        (void)NoteBank_NextSample();
        if(NoteBank_HoldCount()!=0u) break;
        ++frames;
        uint32_t remaining=NoteBank_RemainingUs(0u);
        check(remaining<=last,"remaining time must count down across attack/body");
        last=remaining;
      }
      check(NoteBank_HoldCount()!=0u,"depletion must report an underrun");
      uint32_t actual=(uint32_t)((uint64_t)frames*1000000u/48000u);
      if (!(predicted<=actual && actual-predicted<=22u)) fprintf(stderr,"duration head=%u key=%u predicted=%u actual=%u frames=%u\n",head,key,predicted,actual,frames);
      check(predicted<=actual && actual-predicted<=22u,"reported time predicts depletion within one output frame at each pitch");
    }
  }
  NoteBank_PanicAll();
  check(NoteBank_RemainingUs(0u)==0u,"inactive voice has no deadline");
  check(NoteBank_RefillSamples5ms(0u)==0u,"inactive voice has no refill demand");
  /* Underrun fallback must repeat audio, recover, and remain voice-local. */
  attack_lengths[0]=0u;
  check(NoteBank_NoteOnSampleSession(0u,0u,60u,127u,7u)==0,"start loop test session");
  {int8_t body[USB_STREAM_UAC_BODY_SAMPLES];for(unsigned i=0;i<sizeof body;++i)body[i]=(i&1u)?64:-64;
   check(StreamRing_WriteVoice(0u,7u,1u,0u,body,sizeof body)==sizeof body,"load loop buffer");}
  boundary();NoteBank_HoldCountClear();
  for(unsigned i=0;i<2000u && NoteBank_HoldCount()==0u;++i)(void)NoteBank_NextSample();
  check(NoteBank_HoldCount()!=0u&&NoteBank_IsActive(0u),"underrun keeps voice active");
  {int positive=0,negative=0;
   for(unsigned i=0;i<512u;++i){int32_t sample=NoteBank_NextSample();positive|=sample>0;negative|=sample<0;}
   check(positive&&negative,"empty ring repeats changing audio rather than holding one sample");}
  check(StreamRing_CurrentFill(0u)==0u,"replay does not consume refill capacity");
  {int8_t silence[128]={0};uint32_t misses=NoteBank_HoldCount();
   check(StreamRing_WriteVoice(0u,7u,0u,0u,silence,128u)==128u,"refill after underrun");
   for(unsigned i=0;i<32u;++i)check(NoteBank_NextSample()==0,"refill replaces fallback immediately");
   check(NoteBank_HoldCount()==misses,"refill stops underrun counting");}
  NoteBank_PanicAll();
  check(NoteBank_NoteOnSampleSession(0u,0u,60u,127u,8u)==0,"start empty replacement session");
  {int8_t silence[USB_STREAM_UAC_BODY_SAMPLES]={0};
   check(StreamRing_WriteVoice(0u,8u,1u,0u,silence,sizeof silence)==sizeof silence,"prime silent replacement");}
  boundary();StreamRing_Advance(0u,StreamRing_CurrentFill(0u));
  for(unsigned i=0;i<64u;++i)check(NoteBank_NextSample()==0,"new note cannot replay old note audio");
  {int8_t single=64;
   check(StreamRing_WriteVoice(0u,8u,0u,0u,&single,1u)==1u,"accept single available sample");
   check(render_peak(64u)>0u,"single available sample can repeat without halting");}
  NoteBank_PanicAll();
  free(program);puts("Channel shared Berry VM test passed");return 0;
}
