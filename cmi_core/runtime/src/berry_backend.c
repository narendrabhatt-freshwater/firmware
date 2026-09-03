#include "script/berry_backend.h"

#include "be_exec.h"
#include "be_gc.h"
#include "be_vector.h"
#include "be_vm.h"
#include "berry.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { PHASE_IDLE=0, PHASE_INIT, PHASE_LOAD, PHASE_HANDLER };
typedef struct { uint32_t size, used; } ArenaBlock;
typedef struct { const uint8_t *data; size_t size, position; } MemoryFile;
static ScriptBerryRuntime *s_runtime;
static MemoryFile s_file;
#if defined(__arm__) || defined(__thumb__)
#define SCRIPT_VM_SECTION __attribute__((used, section(".vm_arena"), aligned(8)))
#else
#define SCRIPT_VM_SECTION
#endif
static uint8_t s_upload_scratch[SCRIPT_BERRY_UPLOAD_SIZE] SCRIPT_VM_SECTION;

static size_t align8(size_t n) { return (n+7u)&~(size_t)7u; }
static ArenaBlock *first_block(ScriptBerryRuntime *r) {
  return (ArenaBlock *)(void *)r->arena.bytes;
}
static ArenaBlock *next_block(ScriptBerryRuntime *r, ArenaBlock *b) {
  uint8_t *next=(uint8_t *)(void *)(b+1)+b->size;
  return next+sizeof(*b)<=r->arena.bytes+r->heap_limit ?
         (ArenaBlock *)(void *)next : NULL;
}
static void update_memory(ScriptBerryRuntime *r) {
  uint32_t used=0u, largest=0u; ArenaBlock *b;
  for (b=first_block(r); b!=NULL; b=next_block(r,b)) {
    if (b->used) used+=b->size;
    else if (b->size>largest) largest=b->size;
  }
  r->memory.arena_size=SCRIPT_BERRY_ARENA_SIZE;
  r->memory.arena_current=used;
  if (used>r->memory.arena_peak) r->memory.arena_peak=used;
  r->memory.arena_largest_free=largest;
  r->memory.load_allocations=r->allocations[PHASE_INIT]+r->allocations[PHASE_LOAD];
  r->memory.handler_allocations=r->allocations[PHASE_HANDLER];
  r->memory.load_gc=r->gc_runs[PHASE_INIT]+r->gc_runs[PHASE_LOAD];
  r->memory.handler_gc=r->gc_runs[PHASE_HANDLER];
  r->memory.shared_vm_valid=r->shared_valid;
}
static void arena_reset(ScriptBerryRuntime *r) {
  ArenaBlock *b;
  memset(r->arena.bytes,0,sizeof(r->arena.bytes));
  r->heap_limit=SCRIPT_BERRY_HEAP_SIZE;
  b=first_block(r); b->size=r->heap_limit-(uint32_t)sizeof(*b); b->used=0u;
  memset(r->allocations,0,sizeof(r->allocations));
  memset(r->frees,0,sizeof(r->frees));
  memset(r->gc_runs,0,sizeof(r->gc_runs));
  memset(&r->memory,0,sizeof(r->memory)); update_memory(r);
}
static void *arena_malloc(ScriptBerryRuntime *r, size_t requested) {
  size_t size=align8(requested); ArenaBlock *b;
  if (requested==0u) return NULL;
  if (r->phase==PHASE_HANDLER) {
    ++r->allocations[PHASE_HANDLER]; r->pending_fault=FW_VM_FAULT_ALLOCATION;
    r->discard_vm=1u;
    if (r->abort_active) longjmp(r->abort_jump,1);
    return NULL;
  }
  for (b=first_block(r); b!=NULL; b=next_block(r,b)) if (!b->used&&b->size>=size) {
    if (b->size>=size+sizeof(ArenaBlock)+8u) {
      ArenaBlock *split=(ArenaBlock *)(void *)((uint8_t *)(b+1)+size);
      split->size=b->size-(uint32_t)size-(uint32_t)sizeof(*split); split->used=0u;
      b->size=(uint32_t)size;
    }
    b->used=1u; ++r->allocations[r->phase]; update_memory(r); return b+1;
  }
  return NULL;
}
static void arena_free(ScriptBerryRuntime *r, void *ptr) {
  ArenaBlock *b,*it;
  if (ptr==NULL) return;
  b=(ArenaBlock *)ptr-1;
  if ((uint8_t *)(void *)b<r->arena.bytes ||
      (uint8_t *)(void *)b>=r->arena.bytes+r->heap_limit || !b->used) return;
  b->used=0u; ++r->frees[r->phase];
  for (it=first_block(r); it!=NULL;) {
    ArenaBlock *next=next_block(r,it);
    if (next!=NULL&&!it->used&&!next->used) it->size+=(uint32_t)sizeof(*next)+next->size;
    else it=next;
  }
  update_memory(r);
}
void *script_berry_malloc(size_t n) { return s_runtime?arena_malloc(s_runtime,n):NULL; }
void script_berry_free(void *p) { if (s_runtime) arena_free(s_runtime,p); }
void *script_berry_realloc(void *p,size_t n) {
  ArenaBlock *old; void *fresh;
  if (!s_runtime) return NULL;
  if (!p) return arena_malloc(s_runtime,n);
  if (!n) { arena_free(s_runtime,p); return NULL; }
  if (s_runtime->phase==PHASE_HANDLER) return arena_malloc(s_runtime,n);
  old=(ArenaBlock *)p-1; if (old->size>=align8(n)) return p;
  fresh=arena_malloc(s_runtime,n); if (fresh) { memcpy(fresh,p,old->size); arena_free(s_runtime,p); }
  return fresh;
}

void be_writebuffer(const char *b,size_t n) {
#if !defined(__arm__) && !defined(__thumb__)
  (void)fwrite(b,1u,n,stderr);
#else
  (void)b;(void)n;
#endif
}
char *be_readstring(char *b,size_t n) { (void)b;(void)n;return NULL; }
void *be_fopen(const char *name,const char *mode) {
  (void)mode; if (strcmp(name,"@fwsc-memory")!=0||!s_file.data) return NULL;
  s_file.position=0u; return &s_file;
}
int be_fclose(void *f) { return f==&s_file?0:-1; }
size_t be_fwrite(void *f,const void *b,size_t n) { (void)f;(void)b;(void)n;return 0u; }
size_t be_fread(void *f,void *b,size_t n) {
  size_t left; if (f!=&s_file) return 0u; left=s_file.size-s_file.position;
  if (n>left) n=left;
  memcpy(b,s_file.data+s_file.position,n); s_file.position+=n; return n;
}
char *be_fgets(void *f,void *b,int n) { (void)f;(void)b;(void)n;return NULL; }
int be_fseek(void *f,long o) { if(f!=&s_file||o<0||(size_t)o>s_file.size)return -1;s_file.position=(size_t)o;return 0; }
long be_ftell(void *f) { return f==&s_file?(long)s_file.position:-1; }
long be_fflush(void *f) { (void)f;return 0; }
size_t be_fsize(void *f) { return f==&s_file?s_file.size:0u; }

static void native_error(bvm *vm,FwVmFault fault,const char *message) {
  /* Berry formats protected exceptions through its allocator. Native ABI
   * validation is a known-safe voice-local unwind, so account that work as
   * fault handling rather than as a handler allocation. */
  s_runtime->pending_fault=fault; s_runtime->phase=PHASE_IDLE;
  be_raise(vm,"value_error",message);
}
static void require_count(bvm *vm,int n) {
  if (be_top(vm)!=n) native_error(vm,FW_VM_FAULT_BAD_HOST_ARGUMENT,"invalid argument count");
}
static bint checked_int(bvm *vm,int i,bint lo,bint hi) {
  bint v; if(!be_isint(vm,i))native_error(vm,FW_VM_FAULT_BAD_HOST_ARGUMENT,"integer required");
  v=be_toint(vm,i); if(v<lo||v>hi)native_error(vm,FW_VM_FAULT_BAD_HOST_ARGUMENT,"integer out of range"); return v;
}
static float checked_float(bvm *vm,int i) {
  float v; if(!be_isnumber(vm,i))native_error(vm,FW_VM_FAULT_BAD_HOST_ARGUMENT,"number required");
  v=(float)be_toreal(vm,i); if(!isfinite(v))native_error(vm,FW_VM_FAULT_NONFINITE,"finite number required"); return v;
}
static void require_handler(bvm *vm) {
  if (s_runtime->phase!=PHASE_HANDLER) native_error(vm,FW_VM_FAULT_BAD_HOST_ARGUMENT,"handler context required");
}
static int native_input(bvm *vm) {
  float value=0.0f; bint id; require_handler(vm); require_count(vm,1);
  id=checked_int(vm,1,0,FW_VM_CHANNEL_INPUT_COUNT-1);
  if(!s_runtime->ops.read_input||s_runtime->ops.read_input(s_runtime->ops.context,
     s_runtime->current_voice,(FwVmChannelInput)id,&value)!=0||!isfinite(value))
    native_error(vm,FW_VM_FAULT_HOST_CALL,"input failed");
  be_pushreal(vm,value); be_return(vm);
}
static int native_state_get(bvm *vm) {
  bint slot; require_handler(vm);require_count(vm,1);slot=checked_int(vm,1,0,FW_SCRIPT_CHANNEL_STATE_VALUES-1);
  be_pushreal(vm,s_runtime->state[s_runtime->current_voice][slot]);be_return(vm);
}
static int native_state_set(bvm *vm) {
  bint slot; float value;require_handler(vm);require_count(vm,2);
  slot=checked_int(vm,1,0,FW_SCRIPT_CHANNEL_STATE_VALUES-1);value=checked_float(vm,2);
  s_runtime->state[s_runtime->current_voice][slot]=value;be_pushnil(vm);be_return(vm);
}
static int native_set_amplitude(bvm *vm) {
  float v;require_handler(vm);require_count(vm,1);v=checked_float(vm,1);
  if(!s_runtime->ops.set_amplitude||s_runtime->ops.set_amplitude(s_runtime->ops.context,s_runtime->current_voice,v)!=0)
    native_error(vm,FW_VM_FAULT_HOST_CALL,"set amplitude failed");
  be_pushnil(vm);be_return(vm);
}
static int native_ramp(bvm *vm) {
  float target,slope;require_handler(vm);require_count(vm,2);
  target=checked_float(vm,1);slope=checked_float(vm,2);
  if(!s_runtime->ops.ramp||s_runtime->ops.ramp(s_runtime->ops.context,s_runtime->current_voice,target,slope)!=0)
    native_error(vm,FW_VM_FAULT_HOST_CALL,"ramp failed");
  be_pushnil(vm);be_return(vm);
}
static int native_pitch_for_key(bvm *vm) {
  bint key;float value;require_handler(vm);require_count(vm,1);
  key=checked_int(vm,1,0,FW_SCRIPT_CHANNEL_KEY_COUNT-1);
  value=fw_vm_channel_standard_hz((uint8_t)key);
  if(!isfinite(value)||value<=0.0f)native_error(vm,FW_VM_FAULT_NONFINITE,"pitch lookup failed");
  be_pushreal(vm,value);be_return(vm);
}
static int native_pow(bvm *vm) {
  float base,exponent,value;require_handler(vm);require_count(vm,2);
  base=checked_float(vm,1);exponent=checked_float(vm,2);value=powf(base,exponent);
  if(!isfinite(value))native_error(vm,FW_VM_FAULT_NONFINITE,"pow result must be finite");
  be_pushreal(vm,value);be_return(vm);
}
static int native_noarg(bvm *vm,int (*fn)(void *,uint8_t),const char *message) {
  require_handler(vm);require_count(vm,0);
  if(!fn||fn(s_runtime->ops.context,s_runtime->current_voice)!=0)
    native_error(vm,FW_VM_FAULT_HOST_CALL,message);
  be_pushnil(vm);be_return(vm);
}
static int native_start_note(bvm *vm){
  int argc;float value;require_handler(vm);argc=be_top(vm);
  if(argc<0||argc>1)native_error(vm,FW_VM_FAULT_BAD_HOST_ARGUMENT,"start_note expects zero or one argument");
  if(argc==1){
    value=checked_float(vm,1);
    if(value<=0.0f)native_error(vm,FW_VM_FAULT_BAD_HOST_ARGUMENT,"pitch must be positive");
    if(!s_runtime->ops.start_note_at||s_runtime->ops.start_note_at(s_runtime->ops.context,s_runtime->current_voice,value)!=0)
      native_error(vm,FW_VM_FAULT_HOST_CALL,"start note failed");
  }else{
    if(!s_runtime->ops.start_note||s_runtime->ops.start_note(s_runtime->ops.context,s_runtime->current_voice)!=0)
      native_error(vm,FW_VM_FAULT_HOST_CALL,"start note failed");
  }
  be_pushnil(vm);be_return(vm);
}
static int native_note_end(bvm *vm){return native_noarg(vm,s_runtime->ops.note_end,"note end failed");}
static int native_discard_pending(bvm *vm){return native_noarg(vm,s_runtime->ops.discard_pending,"discard pending failed");}
static int native_led(bvm *vm) {
  float red,green,blue,brightness;require_handler(vm);require_count(vm,4);
  red=checked_float(vm,1);green=checked_float(vm,2);blue=checked_float(vm,3);brightness=checked_float(vm,4);
  if(red<0.0f||red>1.0f||green<0.0f||green>1.0f||blue<0.0f||blue>1.0f||brightness<0.0f||brightness>1.0f)
    native_error(vm,FW_VM_FAULT_BAD_HOST_ARGUMENT,"led values must be between zero and one");
  if(!s_runtime->ops.set_led||s_runtime->ops.set_led(s_runtime->ops.context,s_runtime->current_voice,red,green,blue,brightness)!=0)
    native_error(vm,FW_VM_FAULT_HOST_CALL,"led failed");
  be_pushnil(vm);be_return(vm);
}
static int native_osc(bvm *vm) {
  bint wave;float frequency;uint32_t handle=0u;require_handler(vm);require_count(vm,2);
  wave=checked_int(vm,1,0,7);frequency=checked_float(vm,2);
  if(frequency<=0.0f||frequency>24000.0f)
    native_error(vm,FW_VM_FAULT_BAD_HOST_ARGUMENT,"osc frequency must be greater than 0 and at most 24000 Hz");
  if(!s_runtime->ops.osc||s_runtime->ops.osc(s_runtime->ops.context,s_runtime->current_voice,(uint8_t)wave,frequency,&handle)!=0||handle==0u||handle>INT32_MAX)
    native_error(vm,FW_VM_FAULT_HOST_CALL,"osc configuration failed");
  be_pushint(vm,(bint)handle);be_return(vm);
}

static void observation_hook(bvm *vm,int event,...) {
  ScriptBerryRuntime *r=s_runtime;(void)vm;if(!r)return;
  if(event==BE_OBS_GC_START) {
    ++r->gc_runs[r->phase];
    if(r->phase==PHASE_HANDLER){r->pending_fault=FW_VM_FAULT_GC;r->discard_vm=1u;if(r->abort_active)longjmp(r->abort_jump,1);}
  }
}
static void register_int(bvm *vm,const char *name,bint value){be_pushint(vm,value);be_setglobal(vm,name);be_pop(vm,1);}
static void register_nil(bvm *vm,const char *name){be_pushnil(vm);be_setglobal(vm,name);be_pop(vm,1);}
static void clear_handler_globals(bvm *vm){register_nil(vm,"on_note_on");register_nil(vm,"on_note_off");register_nil(vm,"on_ramp_end");}
static int create_vm(ScriptBerryRuntime *r) {
  bvm *vm; unsigned i;
  arena_reset(r);s_runtime=r;r->phase=PHASE_INIT;r->discard_vm=0u;r->pending_fault=FW_VM_FAULT_NONE;
  vm=be_vm_new();r->vm=vm;if(!vm){r->shared_valid=0u;return -1;}
  be_set_obs_hook(vm,observation_hook);
  if(vm->stacktop-vm->stack<BE_STACK_TOTAL_MAX)be_stack_expansion(vm,BE_STACK_TOTAL_MAX-(int)(vm->stacktop-vm->stack));
  be_vector_resize(vm,&vm->callstack,8);be_vector_clear(&vm->callstack);
  be_regfunc(vm,"input",native_input);be_regfunc(vm,"state_get",native_state_get);be_regfunc(vm,"state_set",native_state_set);
  be_regfunc(vm,"set_amplitude",native_set_amplitude);be_regfunc(vm,"ramp",native_ramp);
  be_regfunc(vm,"start_note",native_start_note);be_regfunc(vm,"note_end",native_note_end);
  be_regfunc(vm,"discard_pending",native_discard_pending);
  be_regfunc(vm,"led",native_led);
  be_regfunc(vm,"osc",native_osc);
  be_regfunc(vm,"pitch_for_key",native_pitch_for_key);be_regfunc(vm,"pow",native_pow);
  register_int(vm,"INPUT_NOTE_ID",FW_VM_CHANNEL_INPUT_NOTE_ID);register_int(vm,"INPUT_FREQUENCY",FW_VM_CHANNEL_INPUT_FREQUENCY);
  register_int(vm,"INPUT_GAIN",FW_VM_CHANNEL_INPUT_GAIN);register_int(vm,"INPUT_GATE",FW_VM_CHANNEL_INPUT_GATE);
  register_int(vm,"INPUT_ACTIVE",FW_VM_CHANNEL_INPUT_ACTIVE);register_int(vm,"INPUT_HAS_PENDING",FW_VM_CHANNEL_INPUT_HAS_PENDING);
  register_int(vm,"INPUT_PENDING_FREQUENCY",FW_VM_CHANNEL_INPUT_PENDING_FREQUENCY);
  register_int(vm,"INPUT_PENDING_GAIN",FW_VM_CHANNEL_INPUT_PENDING_GAIN);register_int(vm,"INPUT_AMPLITUDE",FW_VM_CHANNEL_INPUT_AMPLITUDE);
  register_int(vm,"INPUT_KEY",FW_VM_CHANNEL_INPUT_KEY);register_int(vm,"INPUT_PENDING_KEY",FW_VM_CHANNEL_INPUT_PENDING_KEY);
  register_int(vm,"INPUT_VELOCITY",FW_VM_CHANNEL_INPUT_VELOCITY);register_int(vm,"INPUT_PENDING_VELOCITY",FW_VM_CHANNEL_INPUT_PENDING_VELOCITY);
  clear_handler_globals(vm);
  be_newlist(vm);for(i=0u;i<FW_SCRIPT_CHANNEL_VOICE_COUNT;++i){be_pushnil(vm);be_data_push(vm,-2);be_pop(vm,1);}be_setglobal(vm,"_fw_programs");be_pop(vm,1);
  /* Intern all native exception text before handlers become allocation-free. */
  { static const char *const text[]={"value_error","invalid argument count","integer required","integer out of range","number required","finite number required","handler context required","input failed","set amplitude failed","pow result must be finite","ramp failed","start note failed","note end failed","led values must be between zero and one","led failed","osc frequency must be greater than 0 and at most 24000 Hz","osc configuration failed"};
    for(i=0u;i<sizeof(text)/sizeof(text[0]);++i){be_pushstring(vm,text[i]);be_pop(vm,1);} }
  be_gc_collect(vm);r->phase=PHASE_IDLE;r->shared_valid=1u;r->active_mask=0u;update_memory(r);return 0;
}
static void silence(ScriptBerryRuntime *r,uint8_t voice,FwVmFault fault){
  r->active_mask&=(uint8_t)~(1u<<voice);r->voice_fault[voice]=fault;++r->voice_metrics[voice].faults;
  if(r->ops.silence_voice)r->ops.silence_voice(r->ops.context,voice,fault);
}
static void invalidate_shared(ScriptBerryRuntime *r,FwVmFault fault){
  uint8_t v;r->shared_valid=0u;r->memory.shared_vm_valid=0u;
  for(v=0u;v<FW_SCRIPT_CHANNEL_VOICE_COUNT;++v)silence(r,v,fault);
}
static int push_handler(bvm *vm,uint8_t voice,uint8_t handler){
  if(!be_getglobal(vm,"_fw_programs"))return -1;
  be_pushint(vm,voice);
  if(!be_getindex(vm,-2)){be_pop(vm,3);return -1;}be_remove(vm,-2);be_remove(vm,-2);
  if(!be_islist(vm,-1)){be_pop(vm,1);return -1;}be_pushint(vm,handler);
  if(!be_getindex(vm,-2)){be_pop(vm,3);return -1;}be_remove(vm,-2);be_remove(vm,-2);
  return be_isfunction(vm,-1)?0:-1;
}
static int validate_program(bvm *vm,int index){
  static const bbyte argc[]={2u,0u,0u};unsigned i;int stable=index<0?be_top(vm)+index+1:index;
  if(!be_islist(vm,stable)||be_data_size(vm,stable)!=FW_SCRIPT_CHANNEL_HANDLER_COUNT)return -1;
  for(i=0u;i<FW_SCRIPT_CHANNEL_HANDLER_COUNT;++i){bvalue *value;be_pushint(vm,(bint)i);be_getindex(vm,stable);value=be_indexof(vm,-1);
    if(!var_isclosure(value)||((bclosure *)var_toobj(value))->proto->argc!=argc[i]){be_pop(vm,2);return -1;}be_pop(vm,2);}return 0;
}
static int install_program(bvm *vm,uint8_t voice,int candidate){
  int stable=candidate<0?be_top(vm)+candidate+1:candidate;
  if(!be_getglobal(vm,"_fw_programs"))return -1;
  be_pushint(vm,voice);be_pushvalue(vm,stable);
  if(!be_setindex(vm,-3)){be_pop(vm,3);return -1;}be_pop(vm,3);return 0;
}
static int push_candidate_program(bvm *vm){
  static const char *const names[]={"on_note_on","on_note_off","on_ramp_end"};unsigned i;
  be_newlist(vm);
  for(i=0u;i<FW_SCRIPT_CHANNEL_HANDLER_COUNT;++i){
    bbool found=be_getglobal(vm,names[i]);
    if(!found||!be_isfunction(vm,-1)){be_pop(vm,2);return -1;}
    be_data_push(vm,-2);be_pop(vm,1);
  }
  /* Handler globals exist only while loading. The rooted list is the sole
   * program namespace retained by the shared VM. */
  clear_handler_globals(vm);
  return 0;
}

void script_berry_init(ScriptBerryRuntime *r,const ScriptBerryNativeOps *ops){
  memset(r,0,sizeof(*r));if(ops)r->ops=*ops;for(unsigned v=0;v<FW_SCRIPT_CHANNEL_VOICE_COUNT;++v)r->voice_fault[v]=FW_VM_FAULT_NO_PROGRAM;(void)create_vm(r);
}
void script_berry_stop(ScriptBerryRuntime *r,uint8_t voice){
  bvm *vm;if(voice>=FW_SCRIPT_CHANNEL_VOICE_COUNT)return;vm=(bvm *)r->vm;
  if(r->shared_valid&&vm&&be_getglobal(vm,"_fw_programs")){be_pushint(vm,voice);be_pushnil(vm);(void)be_setindex(vm,-3);be_pop(vm,3);}
  silence(r,voice,FW_VM_FAULT_NO_PROGRAM);
}
void script_berry_stop_all(ScriptBerryRuntime *r){for(uint8_t v=0;v<FW_SCRIPT_CHANNEL_VOICE_COUNT;++v)script_berry_stop(r,v);}
uint8_t script_berry_is_active(const ScriptBerryRuntime *r,uint8_t v){return v<FW_SCRIPT_CHANNEL_VOICE_COUNT&&(r->active_mask&(1u<<v))!=0u;}
uint8_t script_berry_active_mask(const ScriptBerryRuntime *r){return r->active_mask;}
FwVmFault script_berry_fault(const ScriptBerryRuntime *r,uint8_t v){return v<FW_SCRIPT_CHANNEL_VOICE_COUNT?r->voice_fault[v]:FW_VM_FAULT_BAD_HOST_ARGUMENT;}
const FwVmMetrics *script_berry_voice_metrics(const ScriptBerryRuntime *r,uint8_t v){return v<FW_SCRIPT_CHANNEL_VOICE_COUNT?&r->voice_metrics[v]:NULL;}
const FwVmMemoryMetrics *script_berry_memory_metrics(ScriptBerryRuntime *r){update_memory(r);return &r->memory;}
void script_berry_boundary_begin(ScriptBerryRuntime *r){r->boundary_instructions=0u;r->boundary_cycles=0u;}
void script_berry_record_cycles(ScriptBerryRuntime *r,uint8_t v,uint32_t cycles){
  if(v>=FW_SCRIPT_CHANNEL_VOICE_COUNT)return;
  r->boundary_cycles+=cycles;if(cycles>r->voice_metrics[v].boundary_cycles_max)r->voice_metrics[v].boundary_cycles_max=cycles;
}
int script_berry_dispatch(ScriptBerryRuntime *r,FwVmChannelHandler handler,uint8_t voice){
  bvm *vm=(bvm *)r->vm;volatile int result=BE_EXCEPTION;uint32_t used;float first=0.0f,second=0.0f;
  if(!r->shared_valid||!script_berry_is_active(r,voice)||handler>=FW_SCRIPT_CHANNEL_HANDLER_COUNT)return -1;
  if(handler==FW_VM_CHANNEL_HANDLER_NOTE_ON){
    if(!r->ops.read_input||r->ops.read_input(r->ops.context,voice,FW_VM_CHANNEL_INPUT_PENDING_KEY,&first)!=0||
       r->ops.read_input(r->ops.context,voice,FW_VM_CHANNEL_INPUT_PENDING_VELOCITY,&second)!=0||
       !isfinite(first)||first<0.0f||first>127.0f||!isfinite(second)||second<1.0f||second>127.0f){
      silence(r,voice,FW_VM_FAULT_HOST_CALL);return -1;}
  }
  s_runtime=r;r->current_voice=voice;r->pending_fault=FW_VM_FAULT_NONE;r->discard_vm=0u;r->phase=PHASE_HANDLER;
  r->handler_instruction_start=vm->counter_ins;r->abort_active=1u;
  if(setjmp(r->abort_jump)==0){if(push_handler(vm,voice,(uint8_t)handler)==0){
    if(handler==FW_VM_CHANNEL_HANDLER_NOTE_ON){be_pushint(vm,(bint)first);be_pushint(vm,(bint)second);}
    result=be_pcall(vm,handler==FW_VM_CHANNEL_HANDLER_NOTE_ON?2:0);}}
  r->abort_active=0u;used=vm->counter_ins-r->handler_instruction_start;r->phase=PHASE_IDLE;
  if(!r->discard_vm)be_pop(vm,be_top(vm));
  if(r->discard_vm){invalidate_shared(r,r->pending_fault?r->pending_fault:FW_VM_FAULT_SHARED_VM);return -1;}
  if(result!=BE_OK){silence(r,voice,r->pending_fault?r->pending_fault:FW_VM_FAULT_EXCEPTION);return -1;}
  r->boundary_instructions+=used;++r->voice_metrics[voice].dispatches;r->voice_metrics[voice].instructions_total+=used;
  if(used>r->voice_metrics[voice].instructions_max)r->voice_metrics[voice].instructions_max=used;
  return 0;
}

static uint16_t read16(const uint8_t *p){return (uint16_t)p[0]|((uint16_t)p[1]<<8);}
static uint32_t read32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static int parse_header(ScriptBerryRuntime *r){const uint8_t *h=r->upload_header;
  if(memcmp(h,"FWSC",4u)||read16(h+4u)!=FW_SCRIPT_CONTAINER_VERSION||h[6]!=FW_SCRIPT_RUNTIME_BERRY||
     h[7]!=FW_SCRIPT_CONFIG_FLOAT32_INT32||read16(h+8u)!=FW_SCRIPT_CHANNEL_ABI_VERSION||
     read16(h+10u)!=FW_SCRIPT_CONTAINER_HEADER_SIZE)return -1;
  r->upload_expected_size=read32(h+12u);r->upload_expected_crc=read32(h+16u);
  return r->upload_expected_size<=FW_SCRIPT_MAX_PAYLOAD?0:-1;
}
int script_berry_upload_begin(ScriptBerryRuntime *r,uint8_t voice){
  if(voice>=FW_SCRIPT_CHANNEL_VOICE_COUNT||r->upload_active)return -1;
  if(!r->shared_valid&&create_vm(r)!=0)return -1;
  r->upload_active=1u;r->upload_voice=voice;r->upload_header_bytes=0u;r->upload_payload_bytes=0u;
  r->upload_expected_size=0u;r->upload_expected_crc=0u;return 0;
}
int script_berry_upload_feed(ScriptBerryRuntime *r,const void *data,size_t size){
  const uint8_t *p=(const uint8_t *)data;uint8_t *scratch=s_upload_scratch;
  if(!r->upload_active||(!p&&size))return -1;
  while(size&&r->upload_header_bytes<FW_SCRIPT_CONTAINER_HEADER_SIZE){r->upload_header[r->upload_header_bytes++]=*p++;--size;
    if(r->upload_header_bytes==FW_SCRIPT_CONTAINER_HEADER_SIZE&&parse_header(r)!=0){script_berry_upload_abort(r);r->voice_fault[r->upload_voice]=FW_VM_FAULT_BAD_CONTAINER;return -1;}}
  if(r->upload_header_bytes!=FW_SCRIPT_CONTAINER_HEADER_SIZE)return 0;
  if(size>r->upload_expected_size-r->upload_payload_bytes){script_berry_upload_abort(r);return -1;}
  memcpy(scratch+r->upload_payload_bytes,p,size);r->upload_payload_bytes+=(uint32_t)size;return 0;
}
int script_berry_upload_commit(ScriptBerryRuntime *r){
  bvm *vm=(bvm *)r->vm;uint8_t voice=r->upload_voice;uint8_t *scratch=s_upload_scratch;int load,result;
  if(!r->upload_active||r->upload_header_bytes!=FW_SCRIPT_CONTAINER_HEADER_SIZE||r->upload_payload_bytes!=r->upload_expected_size||
     fw_vm_crc32(scratch,r->upload_payload_bytes)!=r->upload_expected_crc){script_berry_upload_abort(r);r->voice_fault[voice]=FW_VM_FAULT_BAD_CONTAINER;return -1;}
  if(r->upload_payload_bytes==0u){script_berry_upload_abort(r);r->voice_fault[voice]=FW_VM_FAULT_BAD_CONTAINER;return -1;}
  s_runtime=r;r->phase=PHASE_LOAD;r->pending_fault=FW_VM_FAULT_NONE;s_file.data=scratch;s_file.size=r->upload_payload_bytes;s_file.position=0u;
  be_pop(vm,be_top(vm));clear_handler_globals(vm);load=be_loadmode(vm,"@fwsc-memory",0);result=load;if(result==BE_OK)result=be_pcall(vm,0);s_file.data=NULL;
  if(result==BE_OK){be_pop(vm,be_top(vm));if(push_candidate_program(vm)!=0)result=BE_EXCEPTION;}
  { int valid=(result==BE_OK)?validate_program(vm,-1):-1;int installed=(result==BE_OK&&valid==0)?install_program(vm,voice,-1):-1;
  if(result!=BE_OK||valid!=0||installed!=0){
#if !defined(__arm__) && !defined(__thumb__)
    if(result!=BE_OK)be_dumpexcept(vm);
#endif
    be_pop(vm,be_top(vm));clear_handler_globals(vm);be_gc_collect(vm);r->phase=PHASE_IDLE;r->upload_active=0u;r->voice_fault[voice]=load==BE_OK?FW_VM_FAULT_BAD_HANDLER:FW_VM_FAULT_BAD_PROGRAM;update_memory(r);return -1;
  }
  }
  be_pop(vm,be_top(vm));memset(r->state[voice],0,sizeof(r->state[voice]));r->active_mask|=(uint8_t)(1u<<voice);r->voice_fault[voice]=FW_VM_FAULT_NONE;
  be_gc_collect(vm);r->phase=PHASE_IDLE;r->upload_active=0u;update_memory(r);return 0;
}
void script_berry_upload_abort(ScriptBerryRuntime *r){r->upload_active=0u;r->upload_header_bytes=0u;r->upload_payload_bytes=0u;}
uint8_t script_berry_upload_is_active(const ScriptBerryRuntime *r,uint8_t v){return r->upload_active&&r->upload_voice==v;}
