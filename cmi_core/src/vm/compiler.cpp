#include "cardlink/vm/compiler.hpp"
#include "freshwater/vm.h"
#include "freshwater/vm_channel.h"
#include "freshwater/vm_source.h"
extern "C" {
#include "be_vm.h"
}
#include "berry.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <cmath>
#include <sstream>

namespace cardlink::vm { namespace {
void Put16(uint8_t *p,uint16_t v){p[0]=static_cast<uint8_t>(v);p[1]=static_cast<uint8_t>(v>>8);}
void Put32(uint8_t *p,uint32_t v){p[0]=static_cast<uint8_t>(v);p[1]=static_cast<uint8_t>(v>>8);p[2]=static_cast<uint8_t>(v>>16);p[3]=static_cast<uint8_t>(v>>24);}
void PutFloat(uint8_t *p,float value){uint32_t bits;std::memcpy(&bits,&value,sizeof(bits));Put32(p,bits);}
uint16_t Get16(const uint8_t *p){return static_cast<uint16_t>(p[0])|static_cast<uint16_t>(p[1])<<8u;}
uint32_t Get32(const uint8_t *p){return static_cast<uint32_t>(p[0])|static_cast<uint32_t>(p[1])<<8u|static_cast<uint32_t>(p[2])<<16u|static_cast<uint32_t>(p[3])<<24u;}
float GetFloat(const uint8_t *p){const uint32_t bits=Get32(p);float value;std::memcpy(&value,&bits,sizeof(value));return value;}
struct InitMetadata { uint8_t keymap[FW_SCRIPT_CHANNEL_KEY_COUNT]; bool tuning_set=false; uint8_t reference_key=60u; float reference_hz=261.625565f; };
thread_local InitMetadata *s_init_metadata=nullptr;
int InitError(bvm *vm,const char *message){be_raise(vm,"value_error",message);return 0;}
int Stub(bvm *vm){
  if(s_init_metadata)return InitError(vm,"runtime function is not available in on_init");
  be_pushnil(vm);be_return(vm);
}
int KeymapSet(bvm *vm){
  if(!s_init_metadata||be_top(vm)!=2||!be_isint(vm,1)||!be_isint(vm,2))return InitError(vm,"keymap_set requires two integer keys");
  const bint input=be_toint(vm,1),output=be_toint(vm,2);
  if(input<0||input>=static_cast<bint>(FW_SCRIPT_CHANNEL_KEY_COUNT)||output<0||output>=static_cast<bint>(FW_SCRIPT_CHANNEL_KEY_COUNT))return InitError(vm,"keymap_set key out of range");
  s_init_metadata->keymap[input]=static_cast<uint8_t>(output);be_pushnil(vm);be_return(vm);
}
int KeymapFill(bvm *vm){
  if(!s_init_metadata||be_top(vm)!=1||!be_isint(vm,1))return InitError(vm,"keymap_fill requires one integer key");
  const bint output=be_toint(vm,1);if(output<0||output>=static_cast<bint>(FW_SCRIPT_CHANNEL_KEY_COUNT))return InitError(vm,"keymap_fill key out of range");
  std::memset(s_init_metadata->keymap,static_cast<int>(output),sizeof(s_init_metadata->keymap));be_pushnil(vm);be_return(vm);
}
int KeymapGet(bvm *vm){
  if(!s_init_metadata||be_top(vm)!=1||!be_isint(vm,1))return InitError(vm,"keymap_get requires one integer key");
  const bint input=be_toint(vm,1);if(input<0||input>=static_cast<bint>(FW_SCRIPT_CHANNEL_KEY_COUNT))return InitError(vm,"keymap_get key out of range");
  be_pushint(vm,s_init_metadata->keymap[input]);be_return(vm);
}
int TuningSet(bvm *vm){
  if(!s_init_metadata||be_top(vm)!=2||!be_isint(vm,1)||!be_isnumber(vm,2))return InitError(vm,"tuning_set requires reference key and frequency");
  const bint key=be_toint(vm,1);const float hz=static_cast<float>(be_toreal(vm,2));
  if(key<0||key>=static_cast<bint>(FW_SCRIPT_CHANNEL_KEY_COUNT)||!std::isfinite(hz)||hz<=0.0f)return InitError(vm,"invalid tuning reference");
  if(s_init_metadata->tuning_set)return InitError(vm,"tuning_set must be called exactly once");
  s_init_metadata->tuning_set=true;s_init_metadata->reference_key=static_cast<uint8_t>(key);s_init_metadata->reference_hz=hz;be_pushnil(vm);be_return(vm);
}
int NativePow(bvm *vm){
  if(be_top(vm)!=2||!be_isnumber(vm,1)||!be_isnumber(vm,2))return InitError(vm,"pow requires two numbers");
  const float value=std::pow(static_cast<float>(be_toreal(vm,1)),static_cast<float>(be_toreal(vm,2)));
  if(!std::isfinite(value))return InitError(vm,"pow result must be finite");be_pushreal(vm,value);be_return(vm);
}
void GlobalInt(bvm *vm,const char *name,bint value){be_pushint(vm,value);be_setglobal(vm,name);be_pop(vm,1);}
void GlobalNil(bvm *vm,const char *name){be_pushnil(vm);be_setglobal(vm,name);be_pop(vm,1);}
void RegisterAbi(bvm *vm){
  const char *functions[]={"input","state_get","state_set","set_amplitude","ramp","hold","start_note","note_end","discard_pending","led"};
  for(const char *name:functions)be_regfunc(vm,name,Stub);
  be_regfunc(vm,"keymap_set",KeymapSet);be_regfunc(vm,"keymap_fill",KeymapFill);
  be_regfunc(vm,"keymap_get",KeymapGet);be_regfunc(vm,"tuning_set",TuningSet);be_regfunc(vm,"pow",NativePow);
  GlobalInt(vm,"INPUT_NOTE_ID",FW_VM_CHANNEL_INPUT_NOTE_ID);GlobalInt(vm,"INPUT_FREQUENCY",FW_VM_CHANNEL_INPUT_FREQUENCY);
  GlobalInt(vm,"INPUT_GAIN",FW_VM_CHANNEL_INPUT_GAIN);GlobalInt(vm,"INPUT_GATE",FW_VM_CHANNEL_INPUT_GATE);
  GlobalInt(vm,"INPUT_ACTIVE",FW_VM_CHANNEL_INPUT_ACTIVE);GlobalInt(vm,"INPUT_HAS_PENDING",FW_VM_CHANNEL_INPUT_HAS_PENDING);
  GlobalInt(vm,"INPUT_PENDING_FREQUENCY",FW_VM_CHANNEL_INPUT_PENDING_FREQUENCY);GlobalInt(vm,"INPUT_PENDING_GAIN",FW_VM_CHANNEL_INPUT_PENDING_GAIN);
  GlobalInt(vm,"INPUT_AMPLITUDE",FW_VM_CHANNEL_INPUT_AMPLITUDE);
  GlobalInt(vm,"INPUT_KEY",FW_VM_CHANNEL_INPUT_KEY);GlobalInt(vm,"INPUT_MAPPED_KEY",FW_VM_CHANNEL_INPUT_MAPPED_KEY);
  GlobalInt(vm,"INPUT_PENDING_KEY",FW_VM_CHANNEL_INPUT_PENDING_KEY);GlobalInt(vm,"INPUT_PENDING_MAPPED_KEY",FW_VM_CHANNEL_INPUT_PENDING_MAPPED_KEY);
  GlobalInt(vm,"INPUT_VELOCITY",FW_VM_CHANNEL_INPUT_VELOCITY);GlobalInt(vm,"INPUT_PENDING_VELOCITY",FW_VM_CHANNEL_INPUT_PENDING_VELOCITY);
  GlobalNil(vm,"on_init");GlobalNil(vm,"on_note_on");GlobalNil(vm,"on_note_off");GlobalNil(vm,"on_ramp_end");
}
CompileResult Error(const std::string &message){CompileResult r;r.message=message;return r;}
bool HandlerHasArity(bvm *vm,const char *name,bbyte argc){
  const bool found=be_getglobal(vm,name)&&be_isfunction(vm,-1);
  bool valid=false;
  if(found){const bvalue *value=be_indexof(vm,-1);valid=var_isclosure(value)&&((bclosure *)var_toobj(value))->proto->argc==argc;}
  be_pop(vm,1);return valid;
}
bool SourceIsIsolated(const std::string &source){
  std::istringstream lines(source);std::string line;
  while(std::getline(lines,line)){
    if(line.find("global ")!=std::string::npos||line.find("import ")!=std::string::npos||line.find("class ")!=std::string::npos)return false;
    const auto first=line.find_first_not_of(" \t\r");if(first==std::string::npos||line[first]=='#'||first!=0u)continue;
    if(line.compare(0,sizeof("def on_init()")-1u,"def on_init()")&&
       line.compare(0,sizeof("def on_note_on(key, velocity)")-1u,"def on_note_on(key, velocity)")&&
       line.compare(0,sizeof("def on_note_off(has_pending)")-1u,"def on_note_off(has_pending)")&&
       line.compare(0,sizeof("def on_ramp_end()")-1u,"def on_ramp_end()")&&
       line.compare(0,3,"end"))return false;
  }
  return true;
}
}

bool ParseChannelProgramMetadata(const uint8_t *program,size_t size,
                                 ChannelProgramMetadata &metadata,
                                 std::string &error){
  if(!program||size<FW_SCRIPT_CONTAINER_HEADER_SIZE+FW_SCRIPT_CHANNEL_METADATA_SIZE){error="FWSC is too short";return false;}
  const uint32_t payload_size=Get32(program+12u);
  if(std::memcmp(program,"FWSC",4u)!=0||Get16(program+4u)!=FW_SCRIPT_CONTAINER_VERSION||
     program[6]!=FW_SCRIPT_RUNTIME_BERRY||program[7]!=FW_SCRIPT_CONFIG_FLOAT32_INT32||
     Get16(program+8u)!=FW_SCRIPT_CHANNEL_ABI_VERSION||Get16(program+10u)!=FW_SCRIPT_CONTAINER_HEADER_SIZE||
     payload_size<=FW_SCRIPT_CHANNEL_METADATA_SIZE||payload_size>FW_SCRIPT_MAX_PAYLOAD||
     size!=FW_SCRIPT_CONTAINER_HEADER_SIZE+payload_size||
     Get32(program+16u)!=fw_vm_crc32(program+FW_SCRIPT_CONTAINER_HEADER_SIZE,payload_size)){
    error="invalid Channel FWSC container";return false;
  }
  const uint8_t *body=program+FW_SCRIPT_CONTAINER_HEADER_SIZE;
  if(body[0]!=FW_SCRIPT_CHANNEL_METADATA_VERSION||body[2]!=0u||body[3]!=0u){error="invalid Channel FWSC metadata";return false;}
  const uint8_t reference_key=body[FW_SCRIPT_CHANNEL_METADATA_REFERENCE_KEY_OFFSET];
  const float reference_hz=GetFloat(body+FW_SCRIPT_CHANNEL_METADATA_REFERENCE_HZ_OFFSET);
  const float standard=fw_vm_channel_standard_hz(reference_key);
  if(reference_key>=FW_SCRIPT_CHANNEL_KEY_COUNT||!std::isfinite(reference_hz)||reference_hz<=0.0f||standard<=0.0f){error="invalid tuning reference";return false;}
  for(unsigned key=0u;key<FW_SCRIPT_CHANNEL_KEY_COUNT;++key){
    const uint8_t mapped=body[FW_SCRIPT_CHANNEL_METADATA_KEYMAP_OFFSET+key];
    if(mapped>=FW_SCRIPT_CHANNEL_KEY_COUNT){error="invalid key map";return false;}
    metadata.keymap[key]=mapped;
  }
  metadata.reference_key=reference_key;metadata.reference_frequency=reference_hz;
  metadata.tuning_scale=reference_hz/standard;error.clear();return true;
}

CompileResult BerryCompiler::CompileChannel(const std::string &source) const {
  char *lowered_data=nullptr;size_t lowered_size=0u;char preprocess_error[256];
  if(fw_vm_preprocess_channel_source(source.data(),source.size(),&lowered_data,&lowered_size,
                                     preprocess_error,sizeof(preprocess_error))!=0)
    return Error(preprocess_error[0]?preprocess_error:"could not preprocess named state");
  std::string lowered(lowered_data,lowered_size);std::free(lowered_data);
  if(!SourceIsIsolated(lowered))return Error("only ABI7 Channel handlers are allowed at top level");
  InitMetadata metadata;for(unsigned key=0;key<FW_SCRIPT_CHANNEL_KEY_COUNT;++key)metadata.keymap[key]=static_cast<uint8_t>(key);
  bvm *vm=be_vm_new();if(!vm)return Error("could not create Berry compiler VM");RegisterAbi(vm);
  if(be_loadbuffer(vm,"channel.be",lowered.data(),lowered.size())!=BE_OK){
    const char *detail=be_tostring(vm,-1);
    const std::string message=detail?std::string("Berry syntax or ABI error: ")+detail:
                                     "Berry syntax or ABI error";
    be_vm_delete(vm);return Error(message);
  }
  const auto stamp=std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const std::string temp="/tmp/fwsc-"+std::to_string(stamp)+".bytecode";
  if(be_savecode(vm,temp.c_str())!=BE_OK){be_vm_delete(vm);return Error("could not serialize Berry bytecode");}
  if(be_pcall(vm,0)!=BE_OK){std::remove(temp.c_str());be_vm_delete(vm);return Error("Berry program initialization failed");}
  const char *handlers[]={"on_note_on","on_note_off","on_ramp_end"};
  const bbyte arities[]={2u,1u,0u};
  for(unsigned i=0u;i<3u;++i)if(!HandlerHasArity(vm,handlers[i],arities[i])){std::remove(temp.c_str());be_vm_delete(vm);return Error(std::string("missing handler or invalid signature: ")+handlers[i]);}
  int init_result=BE_OK;
  const bool got_init=be_getglobal(vm,"on_init")&&be_isfunction(vm,-1);
  if(got_init){
    const bvalue *value=be_indexof(vm,-1);
    if(!var_isclosure(value)||((bclosure *)var_toobj(value))->proto->argc!=0u)init_result=BE_EXCEPTION;
    else{s_init_metadata=&metadata;init_result=be_pcall(vm,0);s_init_metadata=nullptr;}
  }else be_pop(vm,1);
  if(init_result!=BE_OK){std::remove(temp.c_str());be_vm_delete(vm);return Error("on_init failed or has an invalid signature");}
  be_vm_delete(vm);std::ifstream in(temp,std::ios::binary);std::vector<uint8_t> payload((std::istreambuf_iterator<char>(in)),{});in.close();std::remove(temp.c_str());
  if(payload.empty()||payload.size()+FW_SCRIPT_CHANNEL_METADATA_SIZE>FW_SCRIPT_MAX_PAYLOAD)return Error("Berry bytecode and metadata exceed 4096-byte limit");
  std::vector<uint8_t> body(FW_SCRIPT_CHANNEL_METADATA_SIZE+payload.size(),0u);
  body[0]=FW_SCRIPT_CHANNEL_METADATA_VERSION;body[FW_SCRIPT_CHANNEL_METADATA_REFERENCE_KEY_OFFSET]=metadata.reference_key;
  PutFloat(body.data()+FW_SCRIPT_CHANNEL_METADATA_REFERENCE_HZ_OFFSET,metadata.reference_hz);
  std::memcpy(body.data()+FW_SCRIPT_CHANNEL_METADATA_KEYMAP_OFFSET,metadata.keymap,sizeof(metadata.keymap));
  std::memcpy(body.data()+FW_SCRIPT_CHANNEL_METADATA_SIZE,payload.data(),payload.size());
  CompileResult out;out.ok=true;out.message="ok: compiled";out.program.resize(FW_SCRIPT_CONTAINER_HEADER_SIZE+body.size());
  std::memcpy(out.program.data(),"FWSC",4u);Put16(out.program.data()+4u,FW_SCRIPT_CONTAINER_VERSION);
  out.program[6]=FW_SCRIPT_RUNTIME_BERRY;out.program[7]=FW_SCRIPT_CONFIG_FLOAT32_INT32;
  Put16(out.program.data()+8u,FW_SCRIPT_CHANNEL_ABI_VERSION);Put16(out.program.data()+10u,FW_SCRIPT_CONTAINER_HEADER_SIZE);
  Put32(out.program.data()+12u,static_cast<uint32_t>(body.size()));Put32(out.program.data()+16u,fw_vm_crc32(body.data(),body.size()));
  std::memcpy(out.program.data()+FW_SCRIPT_CONTAINER_HEADER_SIZE,body.data(),body.size());return out;
}
CompileResult BerryCompiler::CompileChannelFile(const std::string &path) const {
  std::ifstream input(path);if(!input)return Error("cannot open "+path);std::ostringstream source;source<<input.rdbuf();return CompileChannel(source.str());
}
} // namespace cardlink::vm
