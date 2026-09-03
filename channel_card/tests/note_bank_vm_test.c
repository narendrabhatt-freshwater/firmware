#include "note_bank.h"
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
float AttackBank_GetRootHz(uint16_t id){(void)id;return 260.0f;}
uint32_t AttackBank_GetLen(uint16_t id){return id<ATTACK_BANK_COUNT?attack_lengths[id]:0u;}
const int8_t *AttackBank_Table(uint16_t id){return id<ATTACK_BANK_COUNT?attack_tables[id]:NULL;}
void AttackBank_SetWriteActive(uint8_t active){attack_write_active=active;}
uint8_t AttackBank_WriteIsActive(void){return attack_write_active;}
void AttackBank_Stop(uint8_t note){(void)note;} void AttackBank_StopAll(void){}
int32_t NoteFilter_Process(uint8_t note,int32_t sample){(void)note;last_filter_sample=sample;return sample;}
void NoteFilter_Reset(uint8_t note){(void)note;} void NoteFilter_OnNoteFreq(uint8_t note,double hz){(void)note;(void)hz;}
int ChannelLed_Set(float red,float green,float blue,float brightness){(void)red;(void)green;(void)blue;(void)brightness;return 0;}
static uint8_t *read_file(const char *path,size_t *size){FILE*f=fopen(path,"rb");long n;uint8_t*p;check(f!=NULL,"open FWSC");fseek(f,0,SEEK_END);n=ftell(f);rewind(f);p=malloc((size_t)n);check(p!=NULL&&fread(p,1,(size_t)n,f)==(size_t)n,"read FWSC");fclose(f);*size=(size_t)n;return p;}
static void boundary(void){NoteBank_VmBoundaryBegin();for(unsigned i=0;i<48u;++i)(void)NoteBank_NextSample();NoteBank_VmBoundaryEnd();}
static void boundaries(unsigned count){while(count--)boundary();}
static uint32_t render_peak(unsigned count){uint32_t peak=0u;NoteBank_VmBoundaryBegin();for(unsigned i=0;i<count;++i){int64_t s=NoteBank_NextSample();uint32_t a=(uint32_t)(s<0?-s:s);if(a>peak)peak=a;}NoteBank_VmBoundaryEnd();return peak;}
static void prime_body(uint8_t note){int8_t body[USB_STREAM_UAC_BODY_SAMPLES];for(unsigned i=0u;i<USB_STREAM_UAC_BODY_SAMPLES;++i)body[i]=(int8_t)((i&1u)!=0u?64:-64);check(StreamRing_WriteVoice(note,0xFFu,1u,NoteBank_GetWaveId(note),body,USB_STREAM_UAC_BODY_SAMPLES)==USB_STREAM_UAC_BODY_SAMPLES,"prime production BODY");}
static void prime_silent_body(uint8_t note){int8_t body[USB_STREAM_UAC_BODY_SAMPLES]={0};check(StreamRing_WriteVoice(note,0xFFu,1u,NoteBank_GetWaveId(note),body,USB_STREAM_UAC_BODY_SAMPLES)==USB_STREAM_UAC_BODY_SAMPLES,"prime silent BODY");}
int main(int argc,char **argv){
  uint8_t *program;size_t size;check(argc==4,"program paths required");program=read_file(argv[1],&size);
  NoteEnv_Init();StreamRing_Init();NoteBank_Init();check(NoteBank_VmActiveMask()==0u,"reset has no programs");
  check(NoteBank_NoteOn(0u,60u,127u)==-2,"note reports no program");boundary();check(!NoteBank_IsActive(0u),"no-program silent");
  check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,program,size)==0&&NoteBank_VmUploadCommit(0u)==0,"valid FWSC activates");
  check(NoteBank_VmActiveMask()==1u,"only voice zero loaded");
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
   check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,channel_program,channel_size)==0&&NoteBank_VmUploadCommit(0u)==0,"reload channel example");
   check(NoteBank_NoteOn(0u,60u,32u)==0,"low velocity note accepted");prime_body(0u);boundary();
   boundaries(49u);
   check(NoteBank_GetVelocity(0u)==32u,"low velocity reaches active voice");
   check(NoteEnv_Amplitude(0u)<32.0f/127.0f,"soft velocity attack remains below its peak at 50 ms");
   boundaries(150u);
   check(fabsf(NoteEnv_Amplitude(0u)-(32.0f/127.0f))<0.002f,"soft velocity attack reaches its peak near 200 ms");
   boundaries(500u);
   check(fabsf(NoteEnv_Amplitude(0u)-(32.0f/127.0f*0.2f))<0.0001f,"decay reaches velocity-scaled sustain");
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
   check(fabsf(NoteEnv_Amplitude(0u)-(64.0f/127.0f*0.2f))<0.0001f,"replacement decay reaches sustain");free(channel_program);}
  NoteBank_PanicAll();
  {size_t oscillator_size;uint8_t *oscillator_program=read_file(argv[3],&oscillator_size);
   for(unsigned i=248u;i<256u;++i){attack_lengths[i]=2u;attack_tables[i][0]=64;attack_tables[i][1]=64;}
   check(NoteBank_SetWaveId(0u,247u)==0&&NoteBank_SetWaveId(0u,248u)!=0,"last eight attack IDs must be reserved for oscillators");
   check(NoteBank_SetWaveId(0u,8u)==0,"sample may use a non-oscillator wave ID");
   check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,oscillator_program,oscillator_size)==0&&NoteBank_VmUploadCommit(0u)==0,"load oscillator program");
   check(NoteBank_NoteOn(0u,69u,127u)==0,"oscillator note accepted");prime_silent_body(0u);boundary();
   {uint32_t peak=render_peak(64u);check(NoteBank_IsActive(0u)&&peak>110000000u&&peak<125000000u,"sample and eight oscillators must be averaged before voice gain");
    check(last_filter_sample>900000000&&last_filter_sample<1000000000,"averaged source mix must feed the existing filter");}
   check(NoteBank_NoteOff(0u)==0,"oscillator note off accepted");boundary();check(!NoteBank_IsActive(0u),"oscillator note end clears the voice");free(oscillator_program);}
  attack_lengths[255]=0u;check(NoteBank_NoteOn(0u,69u,127u)==0,"invalid oscillator note posts");prime_silent_body(0u);boundary();
  check(!NoteBank_IsActive(0u)&&!NoteBank_VmIsActive(0u)&&NoteBank_VmFault(0u)==FW_VM_FAULT_HOST_CALL,"unloaded oscillator table must fault and silence only its voice");
  free(program);puts("Channel shared Berry VM test passed");return 0;
}
