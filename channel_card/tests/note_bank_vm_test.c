#include "note_bank.h"
#include "note_envelope.h"
#include "stream_ring.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int ok,const char *message){if(!ok){fprintf(stderr,"%s\n",message);exit(1);}}
float AttackBank_GetRootHz(uint16_t id){(void)id;return 260.0f;}
uint32_t AttackBank_GetLen(uint16_t id){(void)id;return 0u;}
const int16_t *AttackBank_Table(uint16_t id){(void)id;return NULL;}
void AttackBank_Stop(uint8_t note){(void)note;} void AttackBank_StopAll(void){}
int32_t NoteFilter_Process(uint8_t note,int32_t sample){(void)note;return sample;}
void NoteFilter_Reset(uint8_t note){(void)note;} void NoteFilter_OnNoteFreq(uint8_t note,double hz){(void)note;(void)hz;}
int ChannelLed_Set(float red,float green,float blue,float brightness){(void)red;(void)green;(void)blue;(void)brightness;return 0;}
static uint8_t *read_file(const char *path,size_t *size){FILE*f=fopen(path,"rb");long n;uint8_t*p;check(f!=NULL,"open FWSC");fseek(f,0,SEEK_END);n=ftell(f);rewind(f);p=malloc((size_t)n);check(p!=NULL&&fread(p,1,(size_t)n,f)==(size_t)n,"read FWSC");fclose(f);*size=(size_t)n;return p;}
static void put32(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static void refresh_crc(uint8_t *program,size_t size){uint32_t crc=fw_vm_crc32(program+FW_SCRIPT_CONTAINER_HEADER_SIZE,size-FW_SCRIPT_CONTAINER_HEADER_SIZE);put32(program+16u,crc);}
static void boundary(void){NoteBank_VmBoundaryBegin();for(unsigned i=0;i<48u;++i)(void)NoteBank_NextSample();NoteBank_VmBoundaryEnd();}
static uint32_t render_peak(unsigned count){uint32_t peak=0u;NoteBank_VmBoundaryBegin();for(unsigned i=0;i<count;++i){int64_t s=NoteBank_NextSample();uint32_t a=(uint32_t)(s<0?-s:s);if(a>peak)peak=a;}NoteBank_VmBoundaryEnd();return peak;}
int main(int argc,char **argv){
  uint8_t *program;size_t size;check(argc==4,"program paths required");program=read_file(argv[1],&size);
  NoteEnv_Init();StreamRing_Init();NoteBank_Init();check(NoteBank_VmActiveMask()==0u,"reset has no programs");
  check(NoteBank_NoteOn(0u,60u,127u)==-2,"note reports no program");boundary();check(!NoteBank_IsActive(0u),"no-program silent");
  check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,program,size)==0&&NoteBank_VmUploadCommit(0u)==0,"valid FWSC activates");
  check(NoteBank_VmActiveMask()==1u,"only voice zero loaded");check(NoteBank_NoteOn(0u,60u,64u)==0,"note accepted");boundary();
  check(NoteBank_GetKey(0u)==60u&&NoteBank_GetMappedKey(0u)==60u,"identity key map applied");
  check(NoteBank_GetVelocity(0u)==64u,"note velocity applied");
  check(fabs(NoteBank_GetFreq(0u)-261.625565)<0.001,"C4 frequency resolved on card");
  check(NoteBank_IsActive(0u)&&NoteEnv_Amplitude(0u)>0.0f,"Berry starts native attack");
  check(StreamRing_FreeLevel(0u)==0u,"wavetable profile advertises no BODY credit");
  check(NoteBank_SetShape(NOTE_SHAPE_SINE,0.0)==0&&render_peak(256u)>0u,"sine wavetable renders");
  check(NoteBank_SetShape(NOTE_SHAPE_PULSE,0.5)==0&&render_peak(32u)>0u,"pulse oscillator renders");
  check(NoteBank_SetShape(NOTE_SHAPE_TRI,0.5)==0&&render_peak(32u)>0u,"triangle oscillator renders");
  check(NoteBank_SetShape(NOTE_SHAPE_SAW,0.0)==0&&render_peak(32u)>0u,"saw oscillator renders");
  check(NoteBank_VmUploadBegin(1u)==-2,"reload rejected while sounding");
  check(NoteBank_NoteOff(0u)==0,"note off accepted");for(unsigned i=0;i<64u&&NoteBank_IsActive(0u);++i)boundary();
  check(!NoteBank_IsActive(0u),"release ends note");check(NoteBank_VmFaultCount(0u)==0u,"no integration fault");
  check(NoteBank_VmUploadBegin(0u)==0,"replacement begins idle");check(NoteBank_NoteOn(0u,60u,127u)==-3,"note-on rejected while uploading");NoteBank_VmUploadAbort(0u);
  check(NoteBank_VmIsActive(0u),"abort preserves program");
  {uint8_t bad[FW_SCRIPT_CONTAINER_HEADER_SIZE]={0};check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,bad,sizeof(bad))!=0,"bad FWSC rejected");check(NoteBank_VmIsActive(0u),"bad replacement preserved");}
  {uint8_t *bad_crc=malloc(size);check(bad_crc!=NULL,"allocate CRC case");memcpy(bad_crc,program,size);bad_crc[size-1u]^=1u;
   check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,bad_crc,size)==0&&NoteBank_VmUploadCommit(0u)!=0,"CRC failure rejected");
   check(NoteBank_VmIsActive(0u),"CRC failure preserves active program");free(bad_crc);}
  {uint8_t *mapped=malloc(size);uint8_t *metadata;float reference=432.0f;check(mapped!=NULL,"allocate mapped program");memcpy(mapped,program,size);metadata=mapped+FW_SCRIPT_CONTAINER_HEADER_SIZE;
   metadata[FW_SCRIPT_CHANNEL_METADATA_REFERENCE_KEY_OFFSET]=69u;memcpy(metadata+FW_SCRIPT_CHANNEL_METADATA_REFERENCE_HZ_OFFSET,&reference,sizeof(reference));metadata[FW_SCRIPT_CHANNEL_METADATA_KEYMAP_OFFSET+69u]=33u;refresh_crc(mapped,size);
   check(NoteBank_VmUploadBegin(1u)==0&&NoteBank_VmUploadFeed(1u,mapped,size)==0&&NoteBank_VmUploadCommit(1u)==0,"mapped second voice load");free(mapped);}
  check(NoteBank_VmActiveMask()==3u,"per-voice program mask");
  check(NoteBank_NoteOn(1u,69u,1u)==0,"mapped note accepted");boundary();
  check(NoteBank_GetKey(1u)==69u&&NoteBank_GetMappedKey(1u)==33u,"A4 input maps to A1");
  check(NoteBank_GetVelocity(1u)==1u,"minimum velocity survives mapping");
  check(fabs(NoteBank_GetFreq(1u)-54.0)<0.001,"A1 resolves under A4=432 tuning");
  check(NoteBank_NoteOn(0u,69u,127u)==0,"identity voice accepts same physical key");boundary();
  check(NoteBank_GetMappedKey(0u)==69u&&fabs(NoteBank_GetFreq(0u)-440.0)<0.001,"voices retain independent maps and tuning");
  {uint8_t *abi6=malloc(size);int feed;check(abi6!=NULL,"allocate ABI6 case");memcpy(abi6,program,size);abi6[8]=6u;abi6[9]=0u;NoteBank_PanicAll();
   check(NoteBank_VmUploadBegin(0u)==0,"ABI6 upload begin");feed=NoteBank_VmUploadFeed(0u,abi6,size);
   check(feed!=0||NoteBank_VmUploadCommit(0u)!=0,"ABI6 container must be rejected");free(abi6);}
  {size_t example_size;uint8_t *example=read_file(argv[2],&example_size);NoteBank_VmUploadAbort(0u);
   check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,example,example_size)==0&&NoteBank_VmUploadCommit(0u)==0,"load production example");
   check(NoteBank_NoteOn(0u,60u,40u)==0,"example first note");boundary();
   check(StreamRing_HasPending(0u)==0u,"example must start an idle voice immediately");
   check(NoteBank_NoteOn(0u,62u,100u)==0,"example replacement note");boundary();
   check(StreamRing_HasPending(0u)==0u,"example must promote a replacement immediately");free(example);}
  NoteBank_PanicAll();
  {size_t velocity_size;uint8_t *velocity_program=read_file(argv[3],&velocity_size);
   check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,velocity_program,velocity_size)==0&&NoteBank_VmUploadCommit(0u)==0,"load velocity example");
   check(NoteBank_NoteOn(0u,60u,32u)==0,"low velocity note accepted");boundary();
   check(NoteBank_GetVelocity(0u)==32u,"low velocity reaches active voice");
   check(fabsf(NoteEnv_Amplitude(0u)-(32.0f/127.0f))<0.0001f,"velocity example scales amplitude");
   check(NoteBank_NoteOn(0u,62u,127u)==0,"replacement velocity accepted");boundary();
   check(NoteBank_GetVelocity(0u)==127u,"replacement keeps its own velocity");
   check(fabsf(NoteEnv_Amplitude(0u)-1.0f)<0.0001f,"full velocity reaches full envelope amplitude");free(velocity_program);}
  free(program);puts("Channel shared Berry VM test passed");return 0;
}
