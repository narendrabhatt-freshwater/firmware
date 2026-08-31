#include "cardlink/serial_port.hpp"
#include "cardlink/vm/compiler.hpp"
#include "cardlink/vm/uploader.hpp"
#include "freshwater/vm_channel.h"

#include <cstdlib>
#include <fstream>
#include <iostream>

namespace {
void Usage()
{
  std::cerr << "usage:\n"
            << "  fw_vmc compile SOURCE.be -o PROGRAM.fwsc\n"
            << "  fw_vmc upload CDC_PORT VOICE PROGRAM.fwsc [BAUD]\n"
            << "  fw_vmc status CDC_PORT VOICE [BAUD]\n";
}
bool ParseVoice(const char *text, uint8_t &voice)
{
  char *end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);
  if (text == end || *end != '\0' || value >= FW_VM_CHANNEL_VOICE_COUNT)
    return false;
  voice = static_cast<uint8_t>(value);
  return true;
}
}

int main(int argc, char **argv)
{
  if (argc == 5 && std::string(argv[1]) == "compile" &&
      std::string(argv[3]) == "-o") {
    cardlink::vm::BerryCompiler compiler;
    auto result = compiler.CompileChannelFile(argv[2]);
    if (!result.ok) { std::cerr << "fw_vmc: " << result.message << '\n'; return 1; }
    std::ofstream output(argv[4], std::ios::binary);
    output.write(reinterpret_cast<const char *>(result.program.data()),
                 static_cast<std::streamsize>(result.program.size()));
    if (!output) { std::cerr << "fw_vmc: cannot write " << argv[4] << '\n'; return 1; }
    return 0;
  }
  if ((argc == 5 || argc == 6) && std::string(argv[1]) == "upload") {
    uint8_t voice;
    if (!ParseVoice(argv[3], voice)) { std::cerr << "fw_vmc: voice must be 0..7\n"; return 2; }
    const uint32_t baud = argc == 6 ? static_cast<uint32_t>(std::strtoul(argv[5], nullptr, 10)) : 115200u;
    cardlink::SerialPort port;
    if (!port.Open(argv[2], baud)) { std::cerr << port.LastError() << '\n'; return 1; }
    port.SetDtr(true);
    cardlink::vm::VmUploader uploader(port);
    auto result = uploader.UploadFile(voice, argv[4]);
    std::cout << result.message << '\n'; return result.ok ? 0 : 1;
  }
  if ((argc == 4 || argc == 5) && std::string(argv[1]) == "status") {
    uint8_t voice;
    if (!ParseVoice(argv[3], voice)) { std::cerr << "fw_vmc: voice must be 0..7\n"; return 2; }
    const uint32_t baud = argc == 5 ? static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 10)) : 115200u;
    cardlink::SerialPort port;
    if (!port.Open(argv[2], baud)) { std::cerr << port.LastError() << '\n'; return 1; }
    port.SetDtr(true);
    cardlink::vm::VmUploader uploader(port); auto status = uploader.Status(voice);
    std::cout << status.message << '\n'; return status.ok ? 0 : 1;
  }
  Usage(); return 2;
}
