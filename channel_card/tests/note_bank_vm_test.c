#include "note_bank.h"
#include "note_envelope.h"
#include "stream_ring.h"
#include <stdio.h>
#include <stdlib.h>

static void check(int ok,const char *message){if(!ok){fprintf(stderr,"%s\n",message);exit(1);}}
float AttackBank_GetRootHz(uint16_t id){(void)id;return 260.0f;}
uint32_t AttackBank_GetLen(uint16_t id){(void)id;return 0u;}
const int16_t *AttackBank_Table(uint16_t id){(void)id;return NULL;}
void AttackBank_Stop(uint8_t note){(void)note;} void AttackBank_StopAll(void){}
int32_t NoteFilter_Process(uint8_t note,int32_t sample){(void)note;return sample;}
void NoteFilter_Reset(uint8_t note){(void)note;} void NoteFilter_OnNoteFreq(uint8_t note,double hz){(void)note;(void)hz;}
static uint8_t *read_file(const char *path,size_t *size){FILE*f=fopen(path,"rb");long n;uint8_t*p;check(f!=NULL,"open FWSC");fseek(f,0,SEEK_END);n=ftell(f);rewind(f);p=malloc((size_t)n);check(p!=NULL&&fread(p,1,(size_t)n,f)==(size_t)n,"read FWSC");fclose(f);*size=(size_t)n;return p;}
static void boundary(void){NoteBank_VmBoundaryBegin();for(unsigned i=0;i<48u;++i)(void)NoteBank_NextSample();NoteBank_VmBoundaryEnd();}
int main(int argc,char **argv){
  uint8_t *program;size_t size;check(argc==2,"program path required");program=read_file(argv[1],&size);
  NoteEnv_Init();StreamRing_Init();NoteBank_Init();check(NoteBank_VmActiveMask()==0u,"reset has no programs");
  check(NoteBank_SetFreq(0u,260.0,0.5)==-2,"note reports no program");boundary();check(!NoteBank_IsActive(0u),"no-program silent");
  check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,program,size)==0&&NoteBank_VmUploadCommit(0u)==0,"valid FWSC activates");
  check(NoteBank_VmActiveMask()==1u,"only voice zero loaded");check(NoteBank_SetFreq(0u,260.0,0.5)==0,"note accepted");boundary();
  check(NoteBank_IsActive(0u)&&NoteEnv_Amplitude(0u)>0.0f,"Berry starts native attack");
  check(NoteBank_VmUploadBegin(1u)==-2,"reload rejected while sounding");
  check(NoteBank_SetFreq(0u,0.0,0.0)==0,"note off accepted");for(unsigned i=0;i<16u&&NoteBank_IsActive(0u);++i)boundary();
  check(!NoteBank_IsActive(0u),"release ends note");check(NoteBank_VmFaultCount(0u)==0u,"no integration fault");
  check(NoteBank_VmUploadBegin(0u)==0,"replacement begins idle");check(NoteBank_SetFreq(0u,260.0,0.5)==-3,"note-on rejected while uploading");NoteBank_VmUploadAbort(0u);
  check(NoteBank_VmIsActive(0u),"abort preserves program");
  {uint8_t bad[FW_SCRIPT_CONTAINER_HEADER_SIZE]={0};check(NoteBank_VmUploadBegin(0u)==0&&NoteBank_VmUploadFeed(0u,bad,sizeof(bad))!=0,"bad FWSC rejected");check(NoteBank_VmIsActive(0u),"bad replacement preserved");}
  check(NoteBank_VmUploadBegin(1u)==0&&NoteBank_VmUploadFeed(1u,program,size)==0&&NoteBank_VmUploadCommit(1u)==0,"second voice load");
  check(NoteBank_VmActiveMask()==3u,"per-voice program mask");free(program);puts("Channel shared Berry VM test passed");return 0;
}
