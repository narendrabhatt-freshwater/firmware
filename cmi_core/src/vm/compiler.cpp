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
int InitError(bvm *vm,const char *message){be_raise(vm,"value_error",message);return 0;}
int Stub(bvm *vm){
  be_pushnil(vm);be_return(vm);
}
int NativePow(bvm *vm){
  if(be_top(vm)!=2||!be_isnumber(vm,1)||!be_isnumber(vm,2))return InitError(vm,"pow requires two numbers");
  const float value=std::pow(static_cast<float>(be_toreal(vm,1)),static_cast<float>(be_toreal(vm,2)));
  if(!std::isfinite(value))return InitError(vm,"pow result must be finite");be_pushreal(vm,value);be_return(vm);
}
void GlobalInt(bvm *vm,const char *name,bint value){be_pushint(vm,value);be_setglobal(vm,name);be_pop(vm,1);}
void GlobalNil(bvm *vm,const char *name){be_pushnil(vm);be_setglobal(vm,name);be_pop(vm,1);}
void RegisterAbi(bvm *vm){
  const char *functions[]={"input","state_get","state_set","set_amplitude","ramp","start_note","note_end","discard_pending","pitch_for_key","led","osc","route","modulate"};
  for(const char *name:functions)be_regfunc(vm,name,Stub);
  be_regfunc(vm,"pow",NativePow);
  GlobalInt(vm,"INPUT_NOTE_ID",FW_VM_CHANNEL_INPUT_NOTE_ID);GlobalInt(vm,"INPUT_FREQUENCY",FW_VM_CHANNEL_INPUT_FREQUENCY);
  GlobalInt(vm,"INPUT_GAIN",FW_VM_CHANNEL_INPUT_GAIN);GlobalInt(vm,"INPUT_GATE",FW_VM_CHANNEL_INPUT_GATE);
  GlobalInt(vm,"INPUT_ACTIVE",FW_VM_CHANNEL_INPUT_ACTIVE);GlobalInt(vm,"INPUT_HAS_PENDING",FW_VM_CHANNEL_INPUT_HAS_PENDING);
  GlobalInt(vm,"INPUT_PENDING_FREQUENCY",FW_VM_CHANNEL_INPUT_PENDING_FREQUENCY);GlobalInt(vm,"INPUT_PENDING_GAIN",FW_VM_CHANNEL_INPUT_PENDING_GAIN);
  GlobalInt(vm,"INPUT_AMPLITUDE",FW_VM_CHANNEL_INPUT_AMPLITUDE);
  GlobalInt(vm,"INPUT_KEY",FW_VM_CHANNEL_INPUT_KEY);GlobalInt(vm,"INPUT_PENDING_KEY",FW_VM_CHANNEL_INPUT_PENDING_KEY);
  GlobalInt(vm,"INPUT_VELOCITY",FW_VM_CHANNEL_INPUT_VELOCITY);GlobalInt(vm,"INPUT_PENDING_VELOCITY",FW_VM_CHANNEL_INPUT_PENDING_VELOCITY);
  GlobalInt(vm,"OUTPUT",FW_VM_CHANNEL_TARGET_OUTPUT);GlobalInt(vm,"SAMPLE",FW_VM_CHANNEL_TARGET_SAMPLE);
  GlobalInt(vm,"FREQUENCY",FW_VM_CHANNEL_ROUTE_FREQUENCY);GlobalInt(vm,"AMPLITUDE",FW_VM_CHANNEL_ROUTE_AMPLITUDE);
  GlobalNil(vm,"on_note_on");GlobalNil(vm,"on_note_off");GlobalNil(vm,"on_ramp_end");
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
    if(line.compare(0,sizeof("def on_note_on(key, velocity)")-1u,"def on_note_on(key, velocity)")&&
       line.compare(0,sizeof("def on_note_off()")-1u,"def on_note_off()")&&
       line.compare(0,sizeof("def on_ramp_end()")-1u,"def on_ramp_end()")&&
       line.compare(0,3,"end"))return false;
  }
  return true;
}
}

CompileResult BerryCompiler::CompileChannel(const std::string &source) const {
  char *lowered_data=nullptr;size_t lowered_size=0u;char preprocess_error[256];
  if(fw_vm_preprocess_channel_source(source.data(),source.size(),&lowered_data,&lowered_size,
                                     preprocess_error,sizeof(preprocess_error))!=0)
    return Error(preprocess_error[0]?preprocess_error:"could not preprocess named state");
  std::string lowered(lowered_data,lowered_size);std::free(lowered_data);
  if(!SourceIsIsolated(lowered))return Error("only ABI2 Channel handlers are allowed at top level");
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
  const bbyte arities[]={2u,0u,0u};
  for(unsigned i=0u;i<3u;++i)if(!HandlerHasArity(vm,handlers[i],arities[i])){std::remove(temp.c_str());be_vm_delete(vm);return Error(std::string("missing handler or invalid signature: ")+handlers[i]);}
  be_vm_delete(vm);std::ifstream in(temp,std::ios::binary);std::vector<uint8_t> payload((std::istreambuf_iterator<char>(in)),{});in.close();std::remove(temp.c_str());
  if(payload.empty()||payload.size()>FW_SCRIPT_MAX_PAYLOAD)return Error("Berry bytecode exceeds 16384-byte limit");
  CompileResult out;out.ok=true;out.message="ok: compiled";out.program.resize(FW_SCRIPT_CONTAINER_HEADER_SIZE+payload.size());
  std::memcpy(out.program.data(),"FWSC",4u);Put16(out.program.data()+4u,FW_SCRIPT_CONTAINER_VERSION);
  out.program[6]=FW_SCRIPT_RUNTIME_BERRY;out.program[7]=FW_SCRIPT_CONFIG_FLOAT32_INT32;
  Put16(out.program.data()+8u,FW_SCRIPT_CHANNEL_ABI_VERSION);Put16(out.program.data()+10u,FW_SCRIPT_CONTAINER_HEADER_SIZE);
  Put32(out.program.data()+12u,static_cast<uint32_t>(payload.size()));Put32(out.program.data()+16u,fw_vm_crc32(payload.data(),payload.size()));
  std::memcpy(out.program.data()+FW_SCRIPT_CONTAINER_HEADER_SIZE,payload.data(),payload.size());return out;
}
CompileResult BerryCompiler::CompileChannelFile(const std::string &path) const {
  std::ifstream input(path);if(!input)return Error("cannot open "+path);std::ostringstream source;source<<input.rdbuf();return CompileChannel(source.str());
}
} // namespace cardlink::vm
