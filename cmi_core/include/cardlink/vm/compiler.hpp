#ifndef CARDLINK_VM_COMPILER_HPP
#define CARDLINK_VM_COMPILER_HPP
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cardlink::vm {
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
