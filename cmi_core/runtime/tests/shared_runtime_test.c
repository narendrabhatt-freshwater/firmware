#include "script/berry_backend.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif

typedef struct {
  float input[FW_SCRIPT_CHANNEL_VOICE_COUNT][FW_VM_CHANNEL_INPUT_COUNT];
  float last_slope[FW_SCRIPT_CHANNEL_VOICE_COUNT];
  float oscillator_frequency[16];
  uint8_t oscillator_wave[16];
  uint32_t route_source[16];
  int32_t route_target[16];
  float route_gain[16];
  uint8_t route_parameter[16];
  uint32_t ramps, activations, endings, silences, oscillators, routes, output_hash;
} Mock;

static void hash_u32(Mock *m,uint32_t value){m->output_hash=(m->output_hash^value)*UINT32_C(16777619);}
static void hash_float(Mock *m,float value){uint32_t bits;memcpy(&bits,&value,sizeof(bits));hash_u32(m,bits);}

static int read_input(void *c,uint8_t v,FwVmChannelInput i,float *out){Mock*m=c;*out=m->input[v][i];return 0;}
static int set_amplitude(void *c,uint8_t v,float x){Mock*m=c;hash_u32(m,0x10u|v);hash_float(m,x);m->input[v][FW_VM_CHANNEL_INPUT_AMPLITUDE]=x;return 0;}
static int ramp(void *c,uint8_t v,float target,float slope){Mock*m=c;hash_u32(m,0x20u|v);hash_float(m,target);hash_float(m,slope);m->last_slope[v]=slope;++m->ramps;return 0;}
static int activate(void *c,uint8_t v){Mock*m=c;hash_u32(m,0x40u|v);m->input[v][FW_VM_CHANNEL_INPUT_ACTIVE]=1.0f;m->input[v][FW_VM_CHANNEL_INPUT_HAS_PENDING]=0.0f;++m->activations;return 0;}
static int note_end(void *c,uint8_t v){Mock*m=c;hash_u32(m,0x50u|v);m->input[v][FW_VM_CHANNEL_INPUT_ACTIVE]=0.0f;++m->endings;return 0;}
static int set_led(void *c,uint8_t v,float r,float g,float b,float brightness){Mock*m=c;hash_u32(m,0x60u|v);hash_float(m,r);hash_float(m,g);hash_float(m,b);hash_float(m,brightness);return 0;}
static void silence(void *c,uint8_t v,FwVmFault f){Mock*m=c;(void)v;(void)f;++m->silences;}
static int set_osc(void *c,uint8_t v,uint8_t wave,float frequency,uint32_t *handle){Mock*m=c;(void)v;if(m->oscillators>=16u||!handle)return -1;m->oscillator_wave[m->oscillators]=wave;m->oscillator_frequency[m->oscillators]=frequency;*handle=UINT32_C(256)+m->oscillators;++m->oscillators;return 0;}
static int set_route(void *c,uint8_t v,uint32_t source,int32_t target,uint8_t parameter,float gain){Mock*m=c;(void)v;if(m->routes>=16u)return -1;m->route_source[m->routes]=source;m->route_target[m->routes]=target;m->route_parameter[m->routes]=parameter;m->route_gain[m->routes]=gain;++m->routes;return 0;}

static uint8_t *read_file(const char *path,size_t *size){
  FILE *f=fopen(path,"rb");long n;uint8_t *p;assert(f);assert(fseek(f,0,SEEK_END)==0);n=ftell(f);assert(n>0);rewind(f);
  p=malloc((size_t)n);assert(p);assert(fread(p,1,(size_t)n,f)==(size_t)n);fclose(f);*size=(size_t)n;return p;
}
static void upload(ScriptBerryRuntime *r,uint8_t voice,const uint8_t *p,size_t n){
  size_t at=0;int rc;assert(script_berry_upload_begin(r,voice)==0);while(at<n){size_t take=(at*17u+1u)%97u+1u;if(take>n-at)take=n-at;assert(script_berry_upload_feed(r,p+at,take)==0);at+=take;}rc=script_berry_upload_commit(r);if(rc!=0)fprintf(stderr,"upload voice %u failed fault=%u top-valid=%u peak=%u\n",voice,(unsigned)script_berry_fault(r,voice),(unsigned)r->shared_valid,(unsigned)r->memory.arena_peak);assert(rc==0);
}
static int try_upload(ScriptBerryRuntime *r,uint8_t voice,const uint8_t *p,size_t n){
  if(script_berry_upload_begin(r,voice)!=0||script_berry_upload_feed(r,p,n)!=0)return -1;return script_berry_upload_commit(r);
}
static void put16(uint8_t *p,uint16_t value){p[0]=(uint8_t)value;p[1]=(uint8_t)(value>>8);}
static void put32(uint8_t *p,uint32_t value){p[0]=(uint8_t)value;p[1]=(uint8_t)(value>>8);p[2]=(uint8_t)(value>>16);p[3]=(uint8_t)(value>>24);}
static void payload_limits(void){
  ScriptBerryRuntime runtime;ScriptBerryNativeOps ops={0};uint8_t header[FW_SCRIPT_CONTAINER_HEADER_SIZE]={0};
  assert(FW_SCRIPT_MAX_PAYLOAD==16384u&&FW_SCRIPT_CHANNEL_STATE_VALUES==64u);script_berry_init(&runtime,&ops);
  memcpy(header,"FWSC",4u);put16(header+4u,FW_SCRIPT_CONTAINER_VERSION);header[6]=FW_SCRIPT_RUNTIME_BERRY;header[7]=FW_SCRIPT_CONFIG_FLOAT32_INT32;
  put16(header+8u,FW_SCRIPT_CHANNEL_ABI_VERSION);put16(header+10u,FW_SCRIPT_CONTAINER_HEADER_SIZE);put32(header+12u,FW_SCRIPT_MAX_PAYLOAD);
  assert(script_berry_upload_begin(&runtime,0u)==0);assert(script_berry_upload_feed(&runtime,header,sizeof(header))==0);script_berry_upload_abort(&runtime);
  put16(header+8u,1u);assert(script_berry_upload_begin(&runtime,0u)==0);assert(script_berry_upload_feed(&runtime,header,sizeof(header))!=0);
  put16(header+8u,FW_SCRIPT_CHANNEL_ABI_VERSION);
  put32(header+12u,FW_SCRIPT_MAX_PAYLOAD+1u);assert(script_berry_upload_begin(&runtime,0u)==0);assert(script_berry_upload_feed(&runtime,header,sizeof(header))!=0);
}
static void fault_matrix(const char *normal_path,const char *bad_path,const char *nonfinite_path,const char *osc_path,const char *allocation_path){
  const char *paths[4]={bad_path,nonfinite_path,osc_path,allocation_path};
  for(unsigned test=0u;test<4u;++test){ScriptBerryRuntime r;Mock m={0};ScriptBerryNativeOps o={&m,read_input,set_amplitude,ramp,activate,note_end,silence,set_led};size_t normal_size,fault_size;int dispatch;uint8_t *normal=read_file(normal_path,&normal_size),*fault=read_file(paths[test],&fault_size);m.input[0][FW_VM_CHANNEL_INPUT_PENDING_VELOCITY]=100.0f;if(test==1u)m.input[0][FW_VM_CHANNEL_INPUT_FREQUENCY]=INFINITY;script_berry_init(&r,&o);upload(&r,1u,normal,normal_size);upload(&r,0u,fault,fault_size);script_berry_boundary_begin(&r);dispatch=script_berry_dispatch(&r,FW_VM_CHANNEL_HANDLER_NOTE_ON,0u);if(dispatch==0)fprintf(stderr,"fault case %u unexpectedly succeeded\n",test);assert(dispatch!=0);if(test<3u){assert(r.shared_valid&&script_berry_is_active(&r,1u)&&!script_berry_is_active(&r,0u));}else{assert(!r.shared_valid&&script_berry_active_mask(&r)==0u);}free(normal);free(fault);}
}
static void pitch_tracking(const char *path){
  ScriptBerryRuntime runtime;Mock mock={0};ScriptBerryNativeOps ops={&mock,read_input,set_amplitude,ramp,activate,note_end,silence,set_led};
  uint8_t *program;size_t size;program=read_file(path,&size);script_berry_init(&runtime,&ops);upload(&runtime,0u,program,size);
  mock.input[0][FW_VM_CHANNEL_INPUT_PENDING_KEY]=72.0f;
  mock.input[0][FW_VM_CHANNEL_INPUT_PENDING_VELOCITY]=64.0f;
  script_berry_boundary_begin(&runtime);assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_ON,0u)==0);
  assert(fabsf(mock.last_slope[0]-40.0f)<0.0001f);
  assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_OFF,0u)==0);
  assert(fabsf(mock.last_slope[0]-1.0f)<0.0001f);free(program);
}
static void oscillator_config(const char *oscillator_path,const char *two_path,const char *many_path,const char *state_path){
  ScriptBerryRuntime runtime;Mock mock={0};ScriptBerryNativeOps ops={&mock,read_input,set_amplitude,ramp,activate,note_end,silence,set_led,NULL,NULL,set_osc};
  uint8_t *program;size_t size;program=read_file(oscillator_path,&size);script_berry_init(&runtime,&ops);assert(runtime.ops.osc==set_osc);upload(&runtime,0u,program,size);assert(runtime.ops.osc==set_osc);
  mock.input[0][FW_VM_CHANNEL_INPUT_PENDING_KEY]=69.0f;mock.input[0][FW_VM_CHANNEL_INPUT_PENDING_VELOCITY]=127.0f;script_berry_boundary_begin(&runtime);
  {int result=script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_ON,0u);if(result!=0)fprintf(stderr,"oscillator dispatch failed: fault=%u instructions=%u calls=%u activations=%u\n",(unsigned)script_berry_fault(&runtime,0u),(unsigned)runtime.voice_metrics[0].instructions_max,(unsigned)mock.oscillators,(unsigned)mock.activations);assert(result==0);}
  printf("eight_oscillator_instructions=%u\n",(unsigned)runtime.voice_metrics[0].instructions_max);
  assert(mock.oscillators==8u&&runtime.state[0][0]==256.0f);for(unsigned i=0u;i<8u;++i){assert(mock.oscillator_wave[i]==i);assert(fabsf(mock.oscillator_frequency[i]-440.0f*(float)(i+1u))<0.01f);}
  free(program);program=read_file(two_path,&size);script_berry_stop_all(&runtime);mock.oscillators=0u;upload(&runtime,0u,program,size);script_berry_boundary_begin(&runtime);
  assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_ON,0u)==0&&mock.oscillators==2u&&runtime.state[0][0]==256.0f);
  printf("two_oscillator_instructions=%u\n",(unsigned)runtime.voice_metrics[0].instructions_max);
  free(program);program=read_file(many_path,&size);script_berry_stop_all(&runtime);mock.oscillators=0u;upload(&runtime,0u,program,size);script_berry_boundary_begin(&runtime);
  assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_ON,0u)==0&&mock.oscillators==10u);
  printf("ten_oscillator_instructions=%u\n",(unsigned)runtime.voice_metrics[0].instructions_max);
  assert(runtime.memory.handler_allocations==0u&&runtime.memory.handler_gc==0u);
  free(program);program=read_file(state_path,&size);script_berry_stop_all(&runtime);upload(&runtime,0u,program,size);
  mock.input[0][FW_VM_CHANNEL_INPUT_PENDING_KEY]=63.0f;mock.input[0][FW_VM_CHANNEL_INPUT_PENDING_VELOCITY]=127.0f;script_berry_boundary_begin(&runtime);
  assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_ON,0u)==0);
  assert(runtime.state[0][63]==63.0f);free(program);
}
static void routing_config(const char *path){
  ScriptBerryRuntime runtime;Mock mock={0};ScriptBerryNativeOps ops={0};uint8_t *program;size_t size;
  ops.context=&mock;ops.read_input=read_input;ops.start_note=activate;ops.note_end=note_end;ops.silence_voice=silence;ops.osc=set_osc;ops.route=set_route;
  program=read_file(path,&size);script_berry_init(&runtime,&ops);upload(&runtime,0u,program,size);
  mock.input[0][FW_VM_CHANNEL_INPUT_PENDING_KEY]=69.0f;mock.input[0][FW_VM_CHANNEL_INPUT_PENDING_VELOCITY]=127.0f;
  assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_ON,0u)==0);
  assert(mock.oscillators==2u&&mock.routes==2u&&mock.route_source[0]==256u&&mock.route_target[0]==257&&mock.route_parameter[0]==FW_VM_CHANNEL_ROUTE_FREQUENCY&&fabsf(mock.route_gain[0]-250.0f)<0.001f);
  assert(mock.route_source[1]==257u&&mock.route_target[1]==FW_VM_CHANNEL_TARGET_OUTPUT&&mock.route_parameter[1]==FW_VM_CHANNEL_ROUTE_AUDIO&&fabsf(mock.route_gain[1]-0.5f)<0.001f);
  free(program);
}
static void large_program_capacity(const char *path){
  ScriptBerryRuntime runtime;Mock mock={0};ScriptBerryNativeOps ops={&mock,read_input,set_amplitude,ramp,activate,note_end,silence,set_led};
  uint8_t *program;size_t size;const FwVmMemoryMetrics *memory;program=read_file(path,&size);
  assert(size>FW_SCRIPT_CONTAINER_HEADER_SIZE+4096u&&size<=FW_SCRIPT_CONTAINER_HEADER_SIZE+FW_SCRIPT_MAX_PAYLOAD);
  script_berry_init(&runtime,&ops);for(unsigned voice=0u;voice<FW_SCRIPT_CHANNEL_VOICE_COUNT;++voice)upload(&runtime,(uint8_t)voice,program,size);
  for(unsigned reload=0u;reload<32u;++reload)upload(&runtime,3u,program,size);
  memory=script_berry_memory_metrics(&runtime);fprintf(stderr,"eight_large_program_peak=%u heap=%u largest_free=%u\n",memory->arena_peak,(unsigned)SCRIPT_BERRY_HEAP_SIZE,memory->arena_largest_free);
  assert(memory->arena_peak+memory->arena_peak/5u<=SCRIPT_BERRY_HEAP_SIZE);free(program);
}
static void exhausted_replacement_preserves(const char *normal_path,const char *maximum_path){
  ScriptBerryRuntime runtime;Mock mock={0};ScriptBerryNativeOps ops={&mock,read_input,set_amplitude,ramp,activate,note_end,silence,set_led};
  size_t normal_size,maximum_size;uint8_t *normal=read_file(normal_path,&normal_size),*maximum=read_file(maximum_path,&maximum_size);int failed=0;
  assert(maximum_size>14000u&&maximum_size<=FW_SCRIPT_CONTAINER_HEADER_SIZE+FW_SCRIPT_MAX_PAYLOAD);script_berry_init(&runtime,&ops);upload(&runtime,7u,normal,normal_size);
  for(uint8_t voice=0u;voice<7u;++voice){if(try_upload(&runtime,voice,maximum,maximum_size)!=0){failed=1;break;}}
  if(!failed)failed=try_upload(&runtime,7u,maximum,maximum_size)!=0;
  assert(failed&&runtime.shared_valid&&script_berry_is_active(&runtime,7u));free(normal);free(maximum);
}
int main(int argc,char **argv){
  ScriptBerryRuntime runtime;Mock mock={.output_hash=UINT32_C(2166136261)};ScriptBerryNativeOps ops={&mock,read_input,set_amplitude,ramp,activate,note_end,silence,set_led};
  const FwVmMemoryMetrics *mem;uint8_t *program;size_t size;uint32_t i;
  assert(argc==14);program=read_file(argv[1],&size);script_berry_init(&runtime,&ops);assert(runtime.shared_valid);
  for(i=0;i<FW_SCRIPT_CHANNEL_VOICE_COUNT;++i){upload(&runtime,(uint8_t)i,program,size);}
  assert(script_berry_active_mask(&runtime)==0xffu);mem=script_berry_memory_metrics(&runtime);
  printf("eight_program_peak=%u current=%u largest_free=%u\n",mem->arena_peak,mem->arena_current,mem->arena_largest_free);
  for(i=0;i<32u;++i)upload(&runtime,3u,program,size);
  { uint8_t *corrupt=malloc(size);assert(corrupt);memcpy(corrupt,program,size);corrupt[size-1u]^=0x5au;
    assert(script_berry_upload_begin(&runtime,3u)==0);assert(script_berry_upload_feed(&runtime,corrupt,size)==0);
    assert(script_berry_upload_commit(&runtime)!=0);assert(script_berry_is_active(&runtime,3u));free(corrupt); }
  mem=script_berry_memory_metrics(&runtime);
  printf("reload_peak=%u current=%u largest_free=%u\n",mem->arena_peak,mem->arena_current,mem->arena_largest_free);
  for(i=0;i<FW_SCRIPT_CHANNEL_VOICE_COUNT;++i)mock.input[i][FW_VM_CHANNEL_INPUT_PENDING_VELOCITY]=(float)(1u+i*18u);
  script_berry_boundary_begin(&runtime);for(i=0;i<FW_SCRIPT_CHANNEL_VOICE_COUNT;++i)assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_ON,(uint8_t)i)==0);
  assert(mock.activations==FW_SCRIPT_CHANNEL_VOICE_COUNT&&mock.ramps==FW_SCRIPT_CHANNEL_VOICE_COUNT);
  for(i=0;i<FW_SCRIPT_CHANNEL_VOICE_COUNT;++i){
    uint8_t v=(uint8_t)i;script_berry_boundary_begin(&runtime);
    runtime.state[v][0]=1.0f;assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_RAMP_END,v)==0);
    assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_OFF,v)==0);
    assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_RAMP_END,v)==0);
    mock.input[v][FW_VM_CHANNEL_INPUT_ACTIVE]=1.0f;mock.input[v][FW_VM_CHANNEL_INPUT_AMPLITUDE]=0.5f;runtime.state[v][0]=2.0f;
    assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_ON,v)==0);assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_RAMP_END,v)==0);
    mock.input[v][FW_VM_CHANNEL_INPUT_ACTIVE]=1.0f;mock.input[v][FW_VM_CHANNEL_INPUT_AMPLITUDE]=0.0f;runtime.state[v][0]=2.0f;
    assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_ON,v)==0);
    assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_OFF,v)==0);
  }
  for(i=0;i<1000000u;++i){uint8_t v=(uint8_t)(i&7u);if(v==0u)script_berry_boundary_begin(&runtime);assert(script_berry_dispatch(&runtime,FW_VM_CHANNEL_HANDLER_NOTE_ON,v)==0);}
  mem=script_berry_memory_metrics(&runtime);assert(mem->handler_allocations==0u);assert(mem->handler_gc==0u);assert(runtime.shared_valid);
  printf("million_dispatches_ok hash=%08x instructions_max=%u peak=%u recommended_arena=%u\n",mock.output_hash,
    runtime.voice_metrics[0].instructions_max,mem->arena_peak,
    (unsigned)(((mem->arena_peak+SCRIPT_BERRY_UPLOAD_SIZE+
      (mem->arena_peak/5u>2048u?mem->arena_peak/5u:2048u)+1023u)/1024u)*1024u));
  assert(mock.output_hash==UINT32_C(0x687dca85));
  assert(mock.oscillators==0u);
  free(program);fault_matrix(argv[1],argv[2],argv[3],argv[4],argv[5]);pitch_tracking(argv[6]);oscillator_config(argv[7],argv[8],argv[9],argv[10]);routing_config(argv[11]);large_program_capacity(argv[12]);exhausted_replacement_preserves(argv[1],argv[13]);payload_limits();puts("fault_matrix_ok");return 0;
}
