#include "script/berry_backend.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  float input[FW_SCRIPT_CHANNEL_VOICE_COUNT][FW_VM_CHANNEL_INPUT_COUNT];
  float last_slope[FW_SCRIPT_CHANNEL_VOICE_COUNT];
  uint32_t ramps, holds, activations, endings, silences, output_hash;
} Mock;

static void hash_u32(Mock *m,uint32_t value){m->output_hash=(m->output_hash^value)*UINT32_C(16777619);}
static void hash_float(Mock *m,float value){uint32_t bits;memcpy(&bits,&value,sizeof(bits));hash_u32(m,bits);}

static int read_input(void *c,uint8_t v,FwVmChannelInput i,float *out){Mock*m=c;*out=m->input[v][i];return 0;}
static int set_amplitude(void *c,uint8_t v,float x){Mock*m=c;hash_u32(m,0x10u|v);hash_float(m,x);m->input[v][FW_VM_CHANNEL_INPUT_AMPLITUDE]=x;return 0;}
static int ramp(void *c,uint8_t v,float target,float slope){Mock*m=c;hash_u32(m,0x20u|v);hash_float(m,target);hash_float(m,slope);m->last_slope[v]=slope;++m->ramps;return 0;}
static int hold(void *c,uint8_t v){Mock*m=c;hash_u32(m,0x30u|v);++m->holds;return 0;}
static int activate(void *c,uint8_t v){Mock*m=c;hash_u32(m,0x40u|v);m->input[v][FW_VM_CHANNEL_INPUT_ACTIVE]=1.0f;m->input[v][FW_VM_CHANNEL_INPUT_HAS_PENDING]=0.0f;++m->activations;return 0;}
static int note_end(void *c,uint8_t v){Mock*m=c;hash_u32(m,0x50u|v);m->input[v][FW_VM_CHANNEL_INPUT_ACTIVE]=0.0f;++m->endings;return 0;}
static int set_led(void *c,uint8_t v,float r,float g,float b,float brightness){Mock*m=c;hash_u32(m,0x60u|v);hash_float(m,r);hash_float(m,g);hash_float(m,b);hash_float(m,brightness);return 0;}
static void silence(void *c,uint8_t v,FwVmFault f){Mock*m=c;(void)v;(void)f;++m->silences;}

static uint8_t *read_file(const char *path,size_t *size){
  FILE *f=fopen(path,"rb");long n;uint8_t *p;assert(f);assert(fseek(f,0,SEEK_END)==0);n=ftell(f);assert(n>0);rewind(f);
  p=malloc((size_t)n);assert(p);assert(fread(p,1,(size_t)n,f)==(size_t)n);fclose(f);*size=(size_t)n;return p;
}
static void upload(ScriptBerryRuntime *r,uint8_t voice,const uint8_t *p,size_t n){
  size_t at=0;int rc;assert(script_berry_upload_begin(r,voice)==0);while(at<n){size_t take=(at*17u+1u)%97u+1u;if(take>n-at)take=n-at;assert(script_berry_upload_feed(r,p+at,take)==0);at+=take;}rc=script_berry_upload_commit(r);if(rc!=0)fprintf(stderr,"upload voice %u failed fault=%u top-valid=%u peak=%u\n",voice,(unsigned)script_berry_fault(r,voice),(unsigned)r->shared_valid,(unsigned)r->memory.arena_peak);assert(rc==0);
}
static void fault_matrix(const char *normal_path,const char *bad_path,const char *nonfinite_path,const char *allocation_path,const char *runaway_path){
  const char *paths[4]={bad_path,nonfinite_path,allocation_path,runaway_path};
  for(unsigned test=0u;test<4u;++test){ScriptBerryRuntime r;Mock m={0};ScriptBerryNativeOps o={&m,read_input,set_amplitude,ramp,hold,activate,note_end,silence,set_led};size_t normal_size,fault_size;int dispatch;uint8_t *normal=read_file(normal_path,&normal_size),*fault=read_file(paths[test],&fault_size);if(test==1u)m.input[0][FW_VM_CHANNEL_INPUT_FREQUENCY]=INFINITY;script_berry_init(&r,&o);upload(&r,1u,normal,normal_size);upload(&r,0u,fault,fault_size);script_berry_boundary_begin(&r);dispatch=script_berry_dispatch(&r,FW_VM_CHANNEL_HANDLER_NOTE_ON,0u);if(dispatch==0)fprintf(stderr,"fault case %u unexpectedly succeeded\n",test);assert(dispatch!=0);if(test<2u){assert(r.shared_valid&&script_berry_is_active(&r,1u)&&!script_berry_is_active(&r,0u));}else{assert(!r.shared_valid&&script_berry_active_mask(&r)==0u);}free(normal);free(fault);}
}
static void pitch_tracking(const char *path){
  ScriptBerryRuntime runtime;Mock mock={0};ScriptBerryNativeOps ops={&mock,read_input,set_amplitude,ramp,hold,activate,note_end,silence,set_led};
  uint8_t *program;size_t size;program=read_file(path,&size);script_berry_init(&runtime,&ops);upload(&runtime,0u,program,size);
  mock.input[0][FW_VM_CHANNEL_INPUT_PENDING_KEY]=72.0f;
  script_berry_boundary_begin(&runtime);assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_ON,0u)==0);
  assert(fabsf(mock.last_slope[0]-40.0f)<0.0001f);
  assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_OFF,0u)==0);
  assert(fabsf(mock.last_slope[0]-1.0f)<0.0001f);free(program);
}
int main(int argc,char **argv){
  ScriptBerryRuntime runtime;Mock mock={.output_hash=UINT32_C(2166136261)};ScriptBerryNativeOps ops={&mock,read_input,set_amplitude,ramp,hold,activate,note_end,silence,set_led};
  const FwVmMemoryMetrics *mem;uint8_t *program;size_t size;uint32_t i;
  assert(argc==7);program=read_file(argv[1],&size);script_berry_init(&runtime,&ops);assert(runtime.shared_valid);
  for(i=0;i<FW_SCRIPT_CHANNEL_VOICE_COUNT;++i){mock.input[i][FW_VM_CHANNEL_INPUT_CRASH_RELEASE]=3.0f;upload(&runtime,(uint8_t)i,program,size);}
  assert(script_berry_active_mask(&runtime)==0xffu);mem=script_berry_memory_metrics(&runtime);
  printf("eight_program_peak=%u current=%u largest_free=%u\n",mem->arena_peak,mem->arena_current,mem->arena_largest_free);
  for(i=0;i<32u;++i)upload(&runtime,3u,program,size);
  { uint8_t *corrupt=malloc(size);assert(corrupt);memcpy(corrupt,program,size);corrupt[size-1u]^=0x5au;
    assert(script_berry_upload_begin(&runtime,3u)==0);assert(script_berry_upload_feed(&runtime,corrupt,size)==0);
    assert(script_berry_upload_commit(&runtime)!=0);assert(script_berry_is_active(&runtime,3u));free(corrupt); }
  mem=script_berry_memory_metrics(&runtime);
  printf("reload_peak=%u current=%u largest_free=%u\n",mem->arena_peak,mem->arena_current,mem->arena_largest_free);
  script_berry_boundary_begin(&runtime);for(i=0;i<FW_SCRIPT_CHANNEL_VOICE_COUNT;++i)assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_ON,(uint8_t)i)==0);
  assert(mock.activations==FW_SCRIPT_CHANNEL_VOICE_COUNT&&mock.ramps==FW_SCRIPT_CHANNEL_VOICE_COUNT);
  for(i=0;i<FW_SCRIPT_CHANNEL_VOICE_COUNT;++i){
    uint8_t v=(uint8_t)i;script_berry_boundary_begin(&runtime);
    runtime.state[v][0]=1.0f;assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_RAMP_END,v)==0);
    mock.input[v][FW_VM_CHANNEL_INPUT_HAS_PENDING]=0.0f;assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_OFF,v)==0);
    assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_RAMP_END,v)==0);
    mock.input[v][FW_VM_CHANNEL_INPUT_ACTIVE]=1.0f;mock.input[v][FW_VM_CHANNEL_INPUT_AMPLITUDE]=0.5f;runtime.state[v][0]=2.0f;
    assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_ON,v)==0);assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_RAMP_END,v)==0);
    mock.input[v][FW_VM_CHANNEL_INPUT_ACTIVE]=1.0f;mock.input[v][FW_VM_CHANNEL_INPUT_AMPLITUDE]=0.0f;mock.input[v][FW_VM_CHANNEL_INPUT_CRASH_RELEASE]=0.0f;runtime.state[v][0]=2.0f;
    assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_ON,v)==0);
    mock.input[v][FW_VM_CHANNEL_INPUT_HAS_PENDING]=1.0f;assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_OFF,v)==0);
    mock.input[v][FW_VM_CHANNEL_INPUT_HAS_PENDING]=0.0f;mock.input[v][FW_VM_CHANNEL_INPUT_CRASH_RELEASE]=3.0f;
  }
  for(i=0;i<1000000u;++i){uint8_t v=(uint8_t)(i&7u);if(v==0u)script_berry_boundary_begin(&runtime);assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_ON,v)==0);}
  mem=script_berry_memory_metrics(&runtime);assert(mem->handler_allocations==0u);assert(mem->handler_gc==0u);assert(runtime.shared_valid);
  printf("million_dispatches_ok hash=%08x instructions_max=%u peak=%u recommended_arena=%u\n",mock.output_hash,
    runtime.voice_metrics[0].instructions_max,mem->arena_peak,
    (unsigned)(((mem->arena_peak+SCRIPT_BERRY_UPLOAD_SIZE+
      (mem->arena_peak/5u>2048u?mem->arena_peak/5u:2048u)+1023u)/1024u)*1024u));
  assert(mock.output_hash==UINT32_C(0x7db90c25));
  free(program);fault_matrix(argv[1],argv[2],argv[3],argv[4],argv[5]);pitch_tracking(argv[6]);puts("fault_matrix_ok");return 0;
}
