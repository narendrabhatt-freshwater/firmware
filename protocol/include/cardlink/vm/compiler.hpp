#ifndef CARDLINK_VM_COMPILER_HPP
#define CARDLINK_VM_COMPILER_HPP
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cardlink::vm {
struct ChannelProgramMetadata {
  uint8_t reference_key = 60u;
  float reference_frequency = 261.625565f;
  float tuning_scale = 1.0f;
  std::array<uint8_t,128> keymap{};
};
bool ParseChannelProgramMetadata(const uint8_t *program,size_t size,
                                 ChannelProgramMetadata &metadata,
                                 std::string &error);
struct CompileResult {
  bool ok = false;
  std::string message;
  std::vector<uint8_t> program;
};
class BerryCompiler {
public:
  CompileResult CompileChannel(const std::string &source) const;
  CompileResult CompileChannelFile(const std::string &path) const;
};
} // namespace cardlink::vm
#endif
