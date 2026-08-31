#include "cardlink/vm/compiler.hpp"
#include "freshwater/vm.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
static void Check(bool ok,const char *message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
static uint16_t Read16(const uint8_t *p){return static_cast<uint16_t>(p[0])|static_cast<uint16_t>(p[1]<<8);}
static uint32_t Read32(const uint8_t *p){return static_cast<uint32_t>(p[0])|(static_cast<uint32_t>(p[1])<<8)|(static_cast<uint32_t>(p[2])<<16)|(static_cast<uint32_t>(p[3])<<24);}
int main(){
  cardlink::vm::BerryCompiler compiler;auto result=compiler.CompileChannelFile(FW_VM_EXAMPLE_PATH);
  Check(result.ok,"Berry example must compile");Check(result.program.size()>FW_SCRIPT_CONTAINER_HEADER_SIZE,"container size");
  Check(std::memcmp(result.program.data(),"FWSC",4u)==0,"FWSC magic");
  Check(Read16(result.program.data()+4u)==FW_SCRIPT_CONTAINER_VERSION,"container version");
  Check(result.program[6]==FW_SCRIPT_RUNTIME_BERRY&&result.program[7]==FW_SCRIPT_CONFIG_FLOAT32_INT32,"Berry numeric configuration");
  Check(Read16(result.program.data()+8u)==FW_SCRIPT_CHANNEL_ABI_VERSION,"Channel ABI3");
  Check(Read16(result.program.data()+10u)==FW_SCRIPT_CONTAINER_HEADER_SIZE,"header size");
  const uint32_t size=Read32(result.program.data()+12u);Check(size+FW_SCRIPT_CONTAINER_HEADER_SIZE==result.program.size(),"payload size");
  Check(Read32(result.program.data()+16u)==fw_vm_crc32(result.program.data()+FW_SCRIPT_CONTAINER_HEADER_SIZE,size),"payload CRC");
  Check(!compiler.CompileChannel("def on_note_on() end\n").ok,"all handlers required");
  Check(!compiler.CompileChannel("def on_note_on( end\n").ok,"syntax rejected");
  Check(!compiler.CompileChannel("global shared\n").ok,"script globals rejected");
  std::cout<<"Berry compiler/container tests passed\n";return 0;
}
