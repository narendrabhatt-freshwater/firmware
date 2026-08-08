#pragma once

#include "bus_controller.hpp"
#include "env_editor.hpp"
#include "file_dialog.hpp"
#include "log_buffer.hpp"
#include "preview_scope.hpp"
#include "toast.hpp"
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

/** Last-sent Effect toggles (GUI-side until a firmware status poll exists). */
struct EffectUiState
{
  bool phantom = false;
  bool audio_en = false;
  bool led_flash = false;
  bool led_red = false;
  bool led_yellow = false;
  bool echo = false;
  int usb_adc_ch = 1;
  uint8_t adc_chip = 0;
  uint8_t adc_reg = 0;
  uint8_t adc_val = 0;
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
  fw::ui::ToastHost toasts;
  AsyncFileDialog file_dialog;
  AsyncFileDialog::Kind file_dialog_kind = AsyncFileDialog::Kind::Idle;
  int file_dialog_slot = -1; // -1 = folder / same-to-all context

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
  std::deque<std::string> lab_history;
  int lab_history_index = -1;
  char log_filter[64] = {};
  bool show_shortcuts = false;

  int play_mode = 0;
  int wave_slot = 0;
  float wave_rate = 14000.f;
  char wave_cdc_path[256] = {};
  int wave_cdc_port_index = 0;

  bool nav_expanded = true; // legacy settings key; unused by Stitch shell
  float nav_width = 148.f;
  bool log_collapsed = false;
  float layout_log_w = 280.f; // right activity log width
  bool auto_reconnect = false;
  bool settings_dirty = false;
  float settings_save_countdown = 0.f;

  /** Persisted panel sizes for drag splitters (window pixels). */
  float layout_log_h = 100.f; // legacy
  float layout_perform_voice_h = 78.f;
  float layout_perform_piano_h = 140.f;
  float layout_tone_left_w = 0.f; // 0 → seed from fraction on first draw

  int shape_mode = 0;
  float shape_param = 0.5f;
  float filter_hz_f = 20000.f;
  float filter_q_f = 1.f;
  float filter_k_f = 0.f;
  bool filter_bypass = true;
  bool env_apply_all = false;
  int selected_voice = 0;
  int piano_octave = 0; // offset from C4 (0 = C4–C5)
  int piano_velocity = 100;
  float scope_time_div_ms = 10.f;
  float scope_volt_div = 0.25f;
  float peak_hold = 0.f;
  float peak_hold_timer = 0.f;
  bool midi_activity = false;
  float midi_activity_timer = 0.f;

  EffectUiState effect;

  /** Named envelope presets stored next to settings. */
  std::vector<std::pair<std::string, EnvProgram>> env_presets;
  std::vector<EnvProgram> env_undo;
  char env_preset_name[64] = "My patch";

  std::deque<std::string> log_view;
  bool log_auto_scroll = true;

  std::array<float, midi_host::kVoiceCount> voice_glow{};
  std::vector<std::string> pending_drops;

  void RefreshPortLists();
  void Tick();
  void Draw();
  void DrawTopNav();
  void DrawActivityLog();
  void DrawFooter();
  void DrawPerform();
  void DrawSetup();
  void DrawLab();
  void DrawBusFaultBanner();
  void DrawDisconnectedHint(const char *action);
  void ApplyBankEvents(const std::vector<midi_host::BankEvent> &events);
  void AllNotesOff();
  bool EnsureAudio();
  void ShutdownAudio();
  bool ConnectMidi();
  void DisconnectMidi();
  bool ConnectBus();
  void DisconnectBus();
  void RequestConnectBus();
  void HandleKeyboardPiano();
  void MarkSettingsDirty();
  void PushToastOk(const std::string &msg);
  void PushToastErr(const std::string &msg);
  void NotifyEnqueue(bool ok, const char *what);
};
