#include "cardlink/vm/compiler.hpp"
#include "freshwater/vm.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
static void Check(bool ok,const char *message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
static uint16_t Read16(const uint8_t *p){return static_cast<uint16_t>(p[0])|static_cast<uint16_t>(p[1]<<8);}
static uint32_t Read32(const uint8_t *p){return static_cast<uint32_t>(p[0])|(static_cast<uint32_t>(p[1])<<8)|(static_cast<uint32_t>(p[2])<<16)|(static_cast<uint32_t>(p[3])<<24);}
int main(){
  Check(std::fabs(fw_vm_channel_standard_hz(60u)-261.625565f)<0.001f,"standard C4 frequency");
  Check(std::fabs(fw_vm_channel_standard_hz(69u)-440.0f)<0.001f,"standard A4 frequency");
  Check(std::fabs(fw_vm_channel_standard_hz(33u)-55.0f)<0.001f,"standard A1 frequency");
  cardlink::vm::BerryCompiler compiler;auto result=compiler.CompileChannelFile(FW_VM_EXAMPLE_PATH);
  Check(result.ok,"Berry example must compile");Check(result.program.size()>FW_SCRIPT_CONTAINER_HEADER_SIZE,"container size");
  Check(std::memcmp(result.program.data(),"FWSC",4u)==0,"FWSC magic");
  Check(Read16(result.program.data()+4u)==FW_SCRIPT_CONTAINER_VERSION,"container version");
  Check(result.program[6]==FW_SCRIPT_RUNTIME_BERRY&&result.program[7]==FW_SCRIPT_CONFIG_FLOAT32_INT32,"Berry numeric configuration");
  Check(Read16(result.program.data()+8u)==FW_SCRIPT_CHANNEL_ABI_VERSION,"Channel ABI5");
  Check(Read16(result.program.data()+10u)==FW_SCRIPT_CONTAINER_HEADER_SIZE,"header size");
  const uint32_t size=Read32(result.program.data()+12u);Check(size+FW_SCRIPT_CONTAINER_HEADER_SIZE==result.program.size(),"payload size");
  Check(Read32(result.program.data()+16u)==fw_vm_crc32(result.program.data()+FW_SCRIPT_CONTAINER_HEADER_SIZE,size),"payload CRC");
  Check(size>FW_SCRIPT_CHANNEL_METADATA_SIZE,"metadata and bytecode present");
  const uint8_t *metadata=result.program.data()+FW_SCRIPT_CONTAINER_HEADER_SIZE;
  Check(metadata[0]==FW_SCRIPT_CHANNEL_METADATA_VERSION,"metadata version");
  Check(metadata[FW_SCRIPT_CHANNEL_METADATA_REFERENCE_KEY_OFFSET]==60u,"C4 tuning reference");
  Check(metadata[FW_SCRIPT_CHANNEL_METADATA_KEYMAP_OFFSET+69u]==69u,"key map defaults to identity");
  cardlink::vm::ChannelProgramMetadata parsed;std::string parse_error;
  Check(cardlink::vm::ParseChannelProgramMetadata(result.program.data(),result.program.size(),parsed,parse_error),"host metadata parser");
  Check(parsed.reference_key==60u&&std::fabs(parsed.tuning_scale-1.0f)<0.00001f&&parsed.keymap[69u]==69u,"host preview metadata values");
  Check(!compiler.CompileChannel("def on_note_on(key) end\n").ok,"all handlers required");
  Check(!compiler.CompileChannel(
    "def on_init() tuning_set(60, 261.625565) end\ndef on_note_on() end\ndef on_note_off() end\ndef on_ramp_end() end\n").ok,
    "old handler signatures rejected");
  Check(!compiler.CompileChannel("def on_note_on( end\n").ok,"syntax rejected");
  Check(!compiler.CompileChannel("global shared\n").ok,"script globals rejected");
  const char *named_state=
    "def on_init()\n  tuning_set(60, 261.625565)\nend\n"
    "def on_note_on(key)\n"
    "  state stage = 1\n"
    "  stage += 1\n"
    "end\n"
    "def on_note_off(has_pending)\n"
    "  stage = 3\n"
    "end\n"
    "def on_ramp_end()\n"
    "  if stage == 3 hold() end\n"
    "end\n";
  Check(compiler.CompileChannel(named_state).ok,"named persistent state compiles");
  Check(!compiler.CompileChannel(
    "def on_init() tuning_set(60, 261.625565) end\n"
    "def on_note_on(key)\n  state stage\n  state stage\nend\n"
    "def on_note_off(has_pending) end\ndef on_ramp_end() end\n").ok,"duplicate named state rejected");
  const auto mixed_state=compiler.CompileChannel(
    "def on_init() tuning_set(60, 261.625565) end\n"
    "def on_note_on(key)\n  state stage\n  state_set(1, 2)\nend\n"
    "def on_note_off(has_pending) end\ndef on_ramp_end() end\n");
  Check(!mixed_state.ok&&mixed_state.message.find("cannot be mixed")!=std::string::npos,
        "named and numeric state access cannot silently overlap");
  std::ostringstream too_many;
  too_many<<"def on_init() tuning_set(60, 261.625565) end\ndef on_note_on(key)\n";
  for(int i=0;i<17;++i)too_many<<"  state value_"<<i<<"\n";
  too_many<<"end\ndef on_note_off(has_pending) end\ndef on_ramp_end() end\n";
  const auto too_many_result=compiler.CompileChannel(too_many.str());
  Check(!too_many_result.ok&&too_many_result.message.find("maximum is 16")!=std::string::npos,
        "17th named state rejected with limit");
  std::ostringstream oversized;
  oversized<<"def on_init() tuning_set(60, 261.625565) end\ndef on_note_on(key)\n";
  for(int i=0;i<2000;++i)oversized<<"  hold()\n";
  oversized<<"end\ndef on_note_off(has_pending) end\ndef on_ramp_end() end\n";
  const auto oversized_result=compiler.CompileChannel(oversized.str());
  Check(!oversized_result.ok&&oversized_result.message.find("4096-byte limit")!=std::string::npos,
        "oversized generated program rejected with byte limit");
  const char *tail="def on_note_on(key) end\ndef on_note_off(has_pending) end\ndef on_ramp_end() end\n";
  const auto defaults=compiler.CompileChannel(tail);
  Check(defaults.ok,"on_init may be omitted");
  cardlink::vm::ChannelProgramMetadata default_metadata;
  Check(cardlink::vm::ParseChannelProgramMetadata(defaults.program.data(),defaults.program.size(),default_metadata,parse_error)&&
        default_metadata.reference_key==60u&&std::fabs(default_metadata.reference_frequency-261.625565f)<0.001f&&
        default_metadata.keymap[69u]==69u,"omitted on_init uses standard tuning and identity map");
  Check(compiler.CompileChannel(std::string("def on_init() end\n")+tail).ok,
        "on_init may omit tuning_set");
  Check(!compiler.CompileChannel(std::string("def on_init(value) end\n")+tail).ok,
        "optional on_init still requires zero arguments");
  Check(!compiler.CompileChannel(std::string("def on_init() tuning_set(60, 440) tuning_set(69, 440) end\n")+tail).ok,
        "duplicate tuning_set rejected");
  Check(!compiler.CompileChannel(std::string("def on_init() tuning_set(128, 440) end\n")+tail).ok,
        "invalid reference key rejected");
  Check(!compiler.CompileChannel(std::string("def on_init() tuning_set(60, 261.625565) keymap_set(0, 128) end\n")+tail).ok,
        "invalid mapped key rejected");
  const auto remapped=compiler.CompileChannel(
      std::string("def on_init() tuning_set(69, 432) keymap_set(61, 60) end\n")+tail);
  Check(remapped.ok,"black-key remap compiles");
  Check(remapped.program[FW_SCRIPT_CONTAINER_HEADER_SIZE+FW_SCRIPT_CHANNEL_METADATA_KEYMAP_OFFSET+61u]==60u,
        "black-key remap embedded");
  const auto filled=compiler.CompileChannel(
      std::string("def on_init() tuning_set(60, 261.625565) keymap_fill(48) end\n")+tail);
  Check(filled.ok,"all-keys one-pitch map compiles");
  for(unsigned key=0u;key<FW_SCRIPT_CHANNEL_KEY_COUNT;++key)
    Check(filled.program[FW_SCRIPT_CONTAINER_HEADER_SIZE+FW_SCRIPT_CHANNEL_METADATA_KEYMAP_OFFSET+key]==48u,
          "keymap_fill embeds all entries");
  Check(!compiler.CompileChannel(std::string("def on_init() tuning_set(60, 261.625565) ramp(1, 1) end\n")+tail).ok,
        "runtime calls rejected during on_init");
  std::cout<<"Berry compiler/container tests passed\n";return 0;
}
