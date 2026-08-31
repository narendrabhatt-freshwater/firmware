#include "app.hpp"
#include "card_panels.hpp"
#include "theme.hpp"
#include "widgets.hpp"

#include "cardlink/serial_port.hpp"
#include "cardlink/usb/cdc_port.hpp"
#include "cardlink/vm/compiler.hpp"
#include "cardlink/vm/uploader.hpp"
#include "freshwater/vm.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{

using fw::theme::kPalette;
using fw::theme::S;
using fw::theme::S2;
using fw::ui::BtnKind;

struct VmExample
{
  const char *name;
  const char *file;
  const char *description;
};

constexpr VmExample kExamples[] = {
    {"Simple envelope", "simple_envelope", "Immediate start with no crash fade."},
    {"Simple + 2 ms crash", "simple_crash_envelope", "Simple ADSR with a fixed retrigger fade."},
    {"Pitch-tracked envelope", "pitch_tracked_envelope", "Independent pitch tracking per segment."},
    {"Envelope", "current_envelope", "Attack, hold, release, and safe retrigger."},
    {"Gate", "gate", "Immediate full level with a short click-free release."},
    {"Pluck", "pluck", "Fast attack followed by an automatic decay."},
};

void MonoText(const char *text, const ImVec4 &color, ImFont *font)
{
  if (font) ImGui::PushFont(font);
  ImGui::TextColored(color, "%s", text);
  if (font) ImGui::PopFont();
}

bool ReadBytes(const fs::path &path, std::vector<uint8_t> &bytes)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  bytes.assign(std::istreambuf_iterator<char>(input),
               std::istreambuf_iterator<char>());
  return !bytes.empty();
}

bool CachedContainerLooksValid(const std::vector<uint8_t> &bytes)
{
  if (bytes.size() < FW_SCRIPT_CONTAINER_HEADER_SIZE ||
      std::memcmp(bytes.data(), "FWSC", 4u) != 0) return false;
  const uint16_t version = static_cast<uint16_t>(bytes[4]) |
                           (static_cast<uint16_t>(bytes[5]) << 8u);
  const uint16_t abi = static_cast<uint16_t>(bytes[8]) |
                       (static_cast<uint16_t>(bytes[9]) << 8u);
  const uint16_t header = static_cast<uint16_t>(bytes[10]) |
                          (static_cast<uint16_t>(bytes[11]) << 8u);
  const uint32_t payload = static_cast<uint32_t>(bytes[12]) |
      (static_cast<uint32_t>(bytes[13]) << 8u) |
      (static_cast<uint32_t>(bytes[14]) << 16u) |
      (static_cast<uint32_t>(bytes[15]) << 24u);
  const uint32_t crc = static_cast<uint32_t>(bytes[16]) |
      (static_cast<uint32_t>(bytes[17]) << 8u) |
      (static_cast<uint32_t>(bytes[18]) << 16u) |
      (static_cast<uint32_t>(bytes[19]) << 24u);
  return version == FW_SCRIPT_CONTAINER_VERSION &&
         bytes[6] == FW_SCRIPT_RUNTIME_BERRY &&
         bytes[7] == FW_SCRIPT_CONFIG_FLOAT32_INT32 &&
         abi == FW_SCRIPT_CHANNEL_ABI_VERSION &&
         header == FW_SCRIPT_CONTAINER_HEADER_SIZE &&
         payload <= FW_SCRIPT_MAX_PAYLOAD &&
         bytes.size() == FW_SCRIPT_CONTAINER_HEADER_SIZE + payload &&
         crc == fw_vm_crc32(bytes.data() + FW_SCRIPT_CONTAINER_HEADER_SIZE,
                            payload);
}

bool LoadOrCompile(const VmExample &example, std::vector<uint8_t> &program,
                   bool &rebuilt, std::string &error)
{
  const fs::path source = fs::path(CMI_VM_EXAMPLE_SOURCE_DIR) /
                          (std::string(example.file) + ".be");
  const fs::path cache = fs::path(CMI_VM_EXAMPLE_CACHE_DIR) /
                         (std::string(example.file) + ".fwsc");
  std::error_code ec;
  bool fresh = fs::is_regular_file(cache, ec) && !ec;
  if (fresh) {
    const auto source_time = fs::last_write_time(source, ec);
    if (ec) fresh = false;
    const auto cache_time = fresh ? fs::last_write_time(cache, ec)
                                  : fs::file_time_type{};
    if (ec || cache_time < source_time) fresh = false;
  }
  if (fresh && ReadBytes(cache, program) && CachedContainerLooksValid(program)) {
    rebuilt = false;
    return true;
  }

  cardlink::vm::BerryCompiler compiler;
  const auto compiled = compiler.CompileChannelFile(source.string());
  if (!compiled.ok) {
    error = "err: compile " + std::string(example.name) + ": " +
            compiled.message;
    return false;
  }
  program = compiled.program;
  fs::create_directories(cache.parent_path(), ec);
  std::ofstream output(cache, std::ios::binary | std::ios::trunc);
  if (!output || !output.write(reinterpret_cast<const char *>(program.data()),
                               static_cast<std::streamsize>(program.size()))) {
    error = "err: cannot write VM cache " + cache.string();
    return false;
  }
  rebuilt = true;
  return true;
}

void FinishVmLoad(App &app)
{
  VmLoadJob &job = app.vm_load;
  std::string result;
  bool ok = false;
  cardlink::vm::ChannelProgramMetadata metadata;
  {
    std::lock_guard<std::mutex> lock(job.mu);
    if (!job.ready) return;
    result = std::move(job.result);
    ok = job.ok;
    metadata = job.metadata;
    job.ready = false;
  }
  if (job.worker.joinable()) job.worker.join();
  if (result.empty()) return;
  app.log.Push(result);
  if (ok) {
    app.channel_program_metadata = metadata;
    app.PushToastOk(result);
  }
  else app.PushToastErr(result);
}

void StartVmLoad(App &app, const VmExample &example)
{
  VmLoadJob &job = app.vm_load;
  if (job.busy.exchange(true)) return;
  if (job.worker.joinable()) job.worker.join();
  const std::string cdc_path = app.attack_cdc_path;
  {
    std::lock_guard<std::mutex> lock(job.mu);
    job.ready = false;
    job.ok = false;
    job.result.clear();
    job.status = "checking cache";
  }
  job.progress.store(0.01f);
  job.voice.store(-1);

  job.worker = std::thread([&job, example, cdc_path] {
    auto set_status = [&job](const char *status) {
      std::lock_guard<std::mutex> lock(job.mu);
      job.status = status;
    };
    std::vector<uint8_t> program;
    std::string result;
    bool rebuilt = false;
    bool ok = LoadOrCompile(example, program, rebuilt, result);
    cardlink::vm::ChannelProgramMetadata metadata;
    if (ok) ok = cardlink::vm::ParseChannelProgramMetadata(
        program.data(), program.size(), metadata, result);
    if (ok) {
      set_status(rebuilt ? "compiled" : "cached FWSC");
      job.progress.store(0.08f);
      cardlink::SerialPort port;
      std::string open_error;
      ok = cardlink::usb::OpenCdcPort(port, cdc_path, open_error);
      if (!ok) result = open_error;
      if (ok) {
        cardlink::vm::VmUploader uploader(port);
        const auto uploaded = uploader.UploadAll(
            program.data(), program.size(),
            [&job, &set_status](uint8_t voice, float p) {
              job.voice.store(static_cast<int>(voice));
              char status[32];
              std::snprintf(status, sizeof status, "uploading voice %u", voice);
              set_status(status);
              job.progress.store(0.12f +
                  ((static_cast<float>(voice) + p) / 8.f) * 0.88f);
            });
        if (!uploaded.ok) {
          ok = false;
          result = uploaded.message;
        }
      }
      port.Close();
      if (ok) {
        result = "ok: " + std::string(example.name) +
                 (rebuilt ? " compiled and uploaded to voices 0-7"
                          : " uploaded from cache to voices 0-7");
      }
    }
    {
      std::lock_guard<std::mutex> lock(job.mu);
      job.ok = ok;
      if (ok) job.metadata = metadata;
      job.result = std::move(result);
      job.status = ok ? "done" : "failed";
      job.ready = true;
    }
    job.progress.store(ok ? 1.f : job.progress.load());
    job.voice.store(-1);
    job.busy.store(false);
  });
}

} // namespace

void DrawVmProgramCard(App &app)
{
  FinishVmLoad(app);
  ImFont *fs = fw::theme::g_fonts.mono_small;
  const bool busy = app.vm_load.busy.load();
  const bool competing_upload = app.sample_load.busy.load();

  fw::ui::BeginSection("vm_program", "CHANNEL BERRY PROGRAM",
                       ImVec2(0, S(248.f)));
  ImGui::NewLine();
  ImGui::Spacing();

  MonoText("Program", kPalette.text_dim, fs);
  ImGui::SetNextItemWidth(S(220.f));
  ImGui::BeginDisabled(busy);
  if (ImGui::BeginCombo("##vm_program_select",
                        kExamples[app.vm_script_index].name)) {
    for (int i = 0; i < static_cast<int>(std::size(kExamples)); ++i) {
      if (ImGui::Selectable(kExamples[i].name, app.vm_script_index == i))
        app.vm_script_index = i;
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  MonoText(kExamples[app.vm_script_index].description, kPalette.muted, fs);

  ImGui::Spacing();
  MonoText("Channel CDC", kPalette.text_dim, fs);
  MonoText(app.attack_cdc_path[0] ? app.attack_cdc_path : "not selected",
           app.attack_cdc_path[0] ? kPalette.text : kPalette.warning, fs);
  if (fw::ui::Btn("Find CDC", ImVec2(0, S(22.f)), BtnKind::Neutral)) {
    std::string error;
    if (!app.EnsureAttackCdc(error)) app.PushToastErr(error);
  }

  ImGui::Spacing();
  ImGui::BeginDisabled(busy || competing_upload);
  if (fw::ui::Btn(busy ? "Uploading…" : "BUILD + UPLOAD n0–n7",
                  ImVec2(0, S(26.f)), BtnKind::Primary)) {
    std::string error;
    if (!app.EnsureAttackCdc(error)) {
      app.log.Push(error);
      app.PushToastErr(error);
    } else {
      app.log.Push(LogKind::Tx,
                   "tx VM " + std::string(kExamples[app.vm_script_index].name) +
                   " to voices 0-7");
      StartVmLoad(app, kExamples[app.vm_script_index]);
    }
  }
  ImGui::EndDisabled();

  if (busy) {
    std::string status;
    {
      std::lock_guard<std::mutex> lock(app.vm_load.mu);
      status = app.vm_load.status;
    }
    ImGui::SameLine(0.f, S(12.f));
    fw::ui::ProgressBar("vm_upload", app.vm_load.progress.load(),
                        ImVec2(S(130.f), S(4.f)));
    MonoText(status.c_str(), kPalette.accent, fs);
  } else if (competing_upload) {
    MonoText("Wait for the sample upload to finish.", kPalette.warning, fs);
  } else {
    MonoText("Upload silences the card, then programs all eight voices.",
             kPalette.text_dim, fs);
  }

  fw::ui::EndSection();
}
