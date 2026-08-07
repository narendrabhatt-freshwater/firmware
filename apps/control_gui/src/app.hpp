#pragma once

#include "bus_controller.hpp"
#include "env_editor.hpp"
#include "log_buffer.hpp"
#include "preview_scope.hpp"
#include "wave_upload_queue.hpp"

#include "audio_engine.hpp"
#include "midi_input.hpp"
#include "pitch.hpp"
#include "voice_bank.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class GuiView : int
{
  Perform = 0,
  Tone = 1,
  Waves = 2,
  Effect = 3,
  Lab = 4,
  Setup = 5,
};

enum class OutMode : int
{
  Speakers = 0,
  Card = 1,
  Both = 2,
};

struct App
{
  LogBuffer log;
  BusController bus;
  midi_host::VoiceBank bank;
  midi_host::MidiInput midi;
  std::unique_ptr<midi_host::AudioEngine> audio;
  PreviewScope preview;
  std::array<EnvProgram, midi_host::kVoiceCount> voice_envs;
  WaveUploadQueue waves;

  bool midi_open = false;
  bool audio_open = false;
  int midi_port_index = -1;
  std::vector<midi_host::MidiPortInfo> midi_ports;
  std::vector<std::string> serial_ports;
  int serial_port_index = 0;
  char serial_path_buf[256] = {};
  uint32_t baud = 460800;
  int gain_db = 6;
  OutMode out_mode = OutMode::Card;
  GuiView view = GuiView::Perform;

  char raw_cmd[256] = {};
  int raw_target = 0;

  int play_mode = 0;
  int wave_slot = 0;
  float wave_rate = 14000.f;
  char wave_cdc_path[256] = {};
  int wave_cdc_port_index = 0;

  bool nav_expanded = true;
  float nav_width = 148.f;

  int shape_mode = 0;
  float shape_param = 0.5f;
  float filter_hz_f = 20000.f;
  float filter_q_f = 1.f;
  float filter_k_f = 0.f;
  bool filter_bypass = true;
  bool env_apply_all = false;
  int selected_voice = 0;

  std::deque<std::string> log_view;
  bool log_auto_scroll = true;

  std::array<float, midi_host::kVoiceCount> voice_glow{};

  void RefreshPortLists();
  void Tick();
  void Draw();
  void DrawSetup();
  void ApplyBankEvents(const std::vector<midi_host::BankEvent> &events);
  void AllNotesOff();
  bool EnsureAudio();
  void ShutdownAudio();
  bool ConnectMidi();
  void DisconnectMidi();
  bool ConnectBus();
  void DisconnectBus();
};
