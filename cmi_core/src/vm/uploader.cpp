#include "cardlink/vm/uploader.hpp"

#include "cardproto/channel.hpp"
#include "freshwater/vm.h"
#include "freshwater/vm_channel.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

namespace cardlink::vm {
namespace {

void Drain(cardlink::SerialPort &port)
{
  uint8_t buffer[256];
  for (unsigned i = 0; i < 8u; ++i)
    if (port.ReadTimeout(buffer, sizeof(buffer), 5u) == 0u) break;
}

bool Wait(cardlink::SerialPort &port, const std::string &wanted,
          std::string &received, uint32_t timeout_ms)
{
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  uint8_t buffer[256];
  while (std::chrono::steady_clock::now() < deadline) {
    const size_t count = port.ReadTimeout(buffer, sizeof(buffer), 20u);
    received.append(reinterpret_cast<const char *>(buffer), count);
    if (received.find(wanted) != std::string::npos) return true;
    if (received.find("err:") != std::string::npos) return false;
  }
  return false;
}

bool SendNoteOff(cardlink::SerialPort &port, uint8_t voice)
{
  const std::string command = "c:" + cardproto::FormatNoteOff(voice) + "\r";
  port.FlushInput();
  if (!port.Write(reinterpret_cast<const uint8_t *>(command.data()),
                  command.size())) return false;
  port.DrainOutput();
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(1000);
  std::string reply;
  uint8_t bytes[128];
  while (std::chrono::steady_clock::now() < deadline) {
    const size_t count = port.ReadTimeout(bytes, sizeof bytes, 20u);
    reply.append(reinterpret_cast<const char *>(bytes), count);
    if (reply.find("ok") != std::string::npos ||
        reply.find("err:no-program") != std::string::npos) return true;
    if (reply.find("err:") != std::string::npos) return false;
  }
  return false;
}

VmUploadResult Failure(const std::string &message)
{
  VmUploadResult result; result.message = message; return result;
}

uint16_t Read16(const uint8_t *p)
{ return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8); }
uint32_t Read32(const uint8_t *p)
{
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

VmUploader::VmUploader(cardlink::SerialPort &cdc_port) : port_(cdc_port) {}

VmUploadResult VmUploader::Upload(
    uint8_t voice, const uint8_t *program, size_t size,
    const std::function<void(float)> &on_progress)
{
  if (!port_.IsOpen()) return Failure("err: CDC session not open");
  if (voice >= FW_VM_CHANNEL_VOICE_COUNT || program == nullptr ||
      size < FW_SCRIPT_CONTAINER_HEADER_SIZE ||
      size > FW_SCRIPT_CONTAINER_HEADER_SIZE + FW_SCRIPT_MAX_PAYLOAD ||
      std::memcmp(program, "FWSC", 4u) != 0 ||
      Read16(program + 4u) != FW_SCRIPT_CONTAINER_VERSION ||
      program[6] != FW_SCRIPT_RUNTIME_BERRY ||
      program[7] != FW_SCRIPT_CONFIG_FLOAT32_INT32 ||
      Read16(program + 8u) != FW_SCRIPT_CHANNEL_ABI_VERSION ||
      Read16(program + 10u) != FW_SCRIPT_CONTAINER_HEADER_SIZE ||
      Read32(program + 12u) != size - FW_SCRIPT_CONTAINER_HEADER_SIZE ||
      Read32(program + 16u) != fw_vm_crc32(
          program + FW_SCRIPT_CONTAINER_HEADER_SIZE,
          size - FW_SCRIPT_CONTAINER_HEADER_SIZE))
    return Failure("err: invalid FWSC program");
  Drain(port_); port_.FlushInput();
  char command[64];
  std::snprintf(command, sizeof(command), "c:vmload %u %zu\r",
                static_cast<unsigned>(voice), size);
  if (!port_.Write(reinterpret_cast<const uint8_t *>(command),
                   std::strlen(command))) return Failure("err: CDC write vmload");
  port_.DrainOutput();
  std::string received;
  if (!Wait(port_, "ok:ready", received, 2000u))
    return Failure(received.empty() ? "err: no ok:ready" : received);
  if (on_progress) on_progress(0.05f);
  received.clear();
  size_t offset = 0u;
  while (offset < size) {
    const size_t count = std::min<size_t>(512u, size - offset);
    if (!port_.Write(program + offset, count))
      return Failure("err: CDC write VM program");
    offset += count;
    if (on_progress)
      on_progress(0.05f + 0.9f * static_cast<float>(offset) /
                              static_cast<float>(size));
  }
  port_.DrainOutput();
  if (!Wait(port_, "ok:vm", received, 3000u))
    return Failure(received.empty() ? "err: no ok:vm" : received);
  if (on_progress) on_progress(1.0f);
  VmUploadResult result; result.ok = true; result.message = "ok:vm uploaded";
  return result;
}

VmUploadResult VmUploader::UploadFile(
    uint8_t voice, const std::string &path,
    const std::function<void(float)> &on_progress)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) return Failure("err: cannot open " + path);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  return Upload(voice, bytes.data(), bytes.size(), on_progress);
}

VmUploadResult VmUploader::UploadAll(
    const uint8_t *program, size_t size,
    const std::function<void(uint8_t, float)> &on_progress)
{
  const VmStatus preflight = Status(0u);
  if (!preflight.ok)
    return Failure("err: Channel card firmware does not support Berry VM upload");
  for (uint8_t voice = 0u; voice < FW_VM_CHANNEL_VOICE_COUNT; ++voice) {
    if (!SendNoteOff(port_, voice))
      return Failure("err: could not silence voice " +
                     std::to_string(static_cast<unsigned>(voice)));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  for (uint8_t voice = 0u; voice < FW_VM_CHANNEL_VOICE_COUNT; ++voice) {
    const VmUploadResult uploaded = Upload(
        voice, program, size,
        [on_progress, voice](float progress) {
          if (on_progress) on_progress(voice, progress);
        });
    if (!uploaded.ok)
      return Failure("err: voice " +
                     std::to_string(static_cast<unsigned>(voice)) + " " +
                     uploaded.message);
  }
  VmUploadResult result;
  result.ok = true;
  result.message = "ok: uploaded VM program to voices 0-7";
  return result;
}

VmStatus VmUploader::Status(uint8_t voice)
{
  VmStatus status;
  status.voice = voice;
  if (voice >= FW_VM_CHANNEL_VOICE_COUNT) {
    status.message = "err: voice out of range"; return status;
  }
  if (!port_.IsOpen()) { status.message = "err: CDC session not open"; return status; }
  Drain(port_); port_.FlushInput();
  char command[32];
  std::snprintf(command, sizeof(command), "c:vm %u\r",
                static_cast<unsigned>(voice));
  if (!port_.Write(reinterpret_cast<const uint8_t *>(command), std::strlen(command))) {
    status.message = "err: CDC write vm"; return status;
  }
  port_.DrainOutput();
  std::string received;
  if (!Wait(port_, "ok:vm", received, 2000u)) { status.message = received; return status; }
  unsigned long target = 0u; unsigned version = 0u; unsigned long fault = 0u;
  const auto reply = received.find("ok:vm ");
  if (reply == std::string::npos) { status.message = "err: malformed vm status: " + received; return status; }
  const char *line = received.c_str() + reply;
  unsigned reply_voice = 0u;
  if (std::sscanf(line, "ok:vm %u active %lu %u %lu",
                  &reply_voice, &target, &version, &fault) == 4 &&
      reply_voice == voice) {
    status.active = true; status.target_id = static_cast<uint32_t>(target);
    status.target_version = static_cast<uint16_t>(version);
    status.fault = static_cast<uint32_t>(fault);
  } else if (std::sscanf(line, "ok:vm %u inactive %lu",
                         &reply_voice, &fault) == 2 && reply_voice == voice) {
    status.fault = static_cast<uint32_t>(fault);
  } else { status.message = "err: malformed vm status: " + received; return status; }
  status.ok = true; status.message = received; return status;
}

} // namespace cardlink::vm
