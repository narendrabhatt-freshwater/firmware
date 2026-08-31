/**
 * @file main.cpp
 * @brief control_gui — Dear ImGui + GLFW host for MIDI + RS485 console.
 *
 * Portable windowing via GLFW (macOS / Linux / Windows). Protocol, MIDI,
 * audio, and voice allocation are provided by cardlink.
 */

#include "app.hpp"
#include "macos_app.hpp"
#include "product.hpp"
#include "settings.hpp"
#include "theme.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#endif

#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <GLES2/gl2.h>
#elif defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#include <GLFW/glfw3.h>

namespace
{

App *g_app = nullptr;
std::mutex g_drop_mu;
std::vector<std::string> g_drops;

void GlfwErrorCallback(int error, const char *description)
{
  std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

void GlfwDropCallback(GLFWwindow *, int count, const char **paths)
{
  std::lock_guard<std::mutex> lock(g_drop_mu);
  for (int i = 0; i < count; ++i) {
    if (paths[i] && paths[i][0]) {
      g_drops.emplace_back(paths[i]);
    }
  }
}

} // namespace

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  glfwSetErrorCallback(GlfwErrorCallback);
  if (!glfwInit()) {
    std::fprintf(stderr, "glfwInit failed\n");
    return 1;
  }

#if defined(__APPLE__)
  const char *glsl_version = "#version 150";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
  const char *glsl_version = "#version 130";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

  GLFWwindow *window =
      glfwCreateWindow(1360, 900, fw::product::kTitle, nullptr, nullptr);
  if (!window) {
    std::fprintf(stderr, "glfwCreateWindow failed\n");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);
  glfwSetDropCallback(window, GlfwDropCallback);
  fw_macos_apply_branding();

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr; // layout persistence handled by settings.cpp

  App app;
  g_app = &app;
  app.sample_bulk = std::make_unique<cardlink::audio::SampleBulkOut>();
  app.sample_bulk->BindMixer(app.samples.Mixer());
  app.bus.SetPollLog(&app.poll_log);
  /* Stop BODY stream when card finishes release (vq reports idle). */
  app.samples.SetConsole([&app](const std::string &cmd) {
    (void)app.bus.QueueExec(cardproto::Target::Channel, cmd);
  });
  app.samples.SetNoteGate(
      [&app](const cardlink::sample::NoteRequest &note,
             cardlink::sample::Client::NoteGateStart start,
             cardlink::sample::Client::NoteGateDone done) {
        const auto queued = app.bus.QueueChannel(
            [&app, note, start = std::move(start),
             done = std::move(done)](cardproto::ChannelClient &ch) {
              start();
              if (!note.note_on) {
                auto result = ch.NoteOff(note.voice);
                if (result.ok()) app.bus.AcknowledgeSlotOff(note.voice);
                done(result.ok());
                return result;
              }
              char aw[32];
              std::snprintf(aw, sizeof aw, "aw %u %u",
                            static_cast<unsigned>(note.voice),
                            static_cast<unsigned>(note.wave_id));
              auto result = ch.Exec(aw);
              if (result.ok()) {
                result = ch.StreamNoteOn(note.voice, note.key, note.session);
              }
              if (result.ok()) app.bus.AcknowledgeSlotKey(note.voice, note.key);
              done(result.ok());
              return result;
            });
        return queued == BusQueueResult::Ok;
      });
  app.bus.SetIdleHandler([&app](uint8_t slot) {
    if (app.sample_bulk && app.sample_bulk->Running()) {
      app.samples.Silence(slot);
    }
  });
  /* The RS485 worker binds the session to nX; ABI6 vq then grants exact
   * credit for the repeated SOF and steady-state BODY frames. */
  app.bus.SetVqHandler(
      [&app](const cardproto::VoiceQuery &status) {
        if (app.sample_bulk && app.sample_bulk->Running()) {
          app.sample_bulk->SubmitStatus(status);
        }
      });
  fw::settings::Load(app);
  app.samples.SetCdcPath(app.attack_cdc_path);

  // Effective UI scale = persisted user zoom × monitor content scale
  // (content scale is a no-op on macOS — points already handle Retina).
  fw::theme::SetUserZoom(app.ui_scale);
  {
    float sx = 1.f;
    float sy = 1.f;
    glfwGetWindowContentScale(window, &sx, &sy);
    fw::theme::SetContentScale(sx);
  }
  glfwSetWindowContentScaleCallback(
      window, [](GLFWwindow *, float sx, float) {
        fw::theme::SetContentScale(sx);
      });
  (void)fw::theme::ConsumeFontsDirty(); // initial load below uses final scale

  ImGui::StyleColorsDark();
  fw::theme::Apply();
  fw::theme::LoadFonts(io);
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);
  app.RefreshPortLists();
  {
    char ready[160];
    std::snprintf(ready, sizeof(ready),
                  "%s %s ready — connect MIDI and/or RS485, start from Perform",
                  fw::product::kTitle, fw::product::kVersion);
    app.log.Push(ready);
  }
  if (app.auto_reconnect && app.serial_path_buf[0]) {
    app.RequestConnectBus();
  }

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    // Zoom / content-scale changed: rebake fonts + style outside the frame.
    if (fw::theme::ConsumeFontsDirty()) {
      ImGui_ImplOpenGL3_DestroyFontsTexture();
      fw::theme::LoadFonts(io);
      fw::theme::Apply();
      ImGui_ImplOpenGL3_CreateFontsTexture();
    }

    {
      std::lock_guard<std::mutex> lock(g_drop_mu);
      if (!g_drops.empty()) {
        app.pending_drops.insert(app.pending_drops.end(), g_drops.begin(),
                                 g_drops.end());
        g_drops.clear();
      }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Space) && !io.WantTextInput) {
      app.AllNotesOff();
    }

    app.Tick();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    app.Draw();
    ImGui::Render();

    int fb_w = 0;
    int fb_h = 0;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(fw::theme::kPalette.bg.x, fw::theme::kPalette.bg.y,
                 fw::theme::kPalette.bg.z, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
  }

  fw::settings::Save(app);
  app.AllNotesOff();
  app.DisconnectMidi();
  /* Keep the BODY consumer alive until the bus worker can no longer enqueue
   * vq/idle mixer commands. Otherwise Close() can wait on a saturated queue. */
  app.DisconnectBus();
  if (app.sample_bulk) {
    app.sample_bulk->Stop();
  }
  app.ShutdownAudio();
  g_app = nullptr;

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
