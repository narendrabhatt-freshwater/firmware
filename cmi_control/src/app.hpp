#pragma once

#include "bus_controller.hpp"
#include "env_editor.hpp"
#include "file_dialog.hpp"
#include "log_buffer.hpp"
#include "preview_scope.hpp"
#include "toast.hpp"
#include "cardlink/audio/sample_uac.hpp"
#include "cardlink/sample/client.hpp"

#include "cardlink/audio/audio_engine.hpp"
#include "cardlink/midi/midi_input.hpp"
#include "cardlink/midi/pitch.hpp"
#include "cardlink/midi/voice_bank.hpp"

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
  Sample = 2,
  Effect = 3,
  Setup = 4,
};

enum class OutMode : int
{
  Speakers = 0,
  Card = 1,
  Both = 2,
};

/** Native picker target for the SAMPLE page. */
enum class SampleFilePick : int8_t
{
  None = 0,
  Wave = 1,
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
  int adc_mode = 0; // 0 = read, 1 = write
};

struct App
{
  LogBuffer log;
  /** vq / ring-fill chatter — separate from activity log. */
  LogBuffer poll_log;
  BusController bus;
  cardlink::midi::VoiceBank bank;
  cardlink::midi::MidiInput midi;
  std::unique_ptr<cardlink::audio::AudioEngine> audio;
  PreviewScope preview;
  std::array<EnvProgram, cardlink::midi::kVoiceCount> voice_envs;
  cardlink::sample::Client samples;
  std::unique_ptr<cardlink::audio::SampleUacOut> sample_uac;
  fw::ui::ToastHost toasts;
  AsyncFileDialog file_dialog;
  SampleFilePick sample_file_pick = SampleFilePick::None;
  int sample_file_voice = -1;
  std::string pending_sample_folder; /**< Pending wave pick: `wave:<v>:<path>`. */

  bool midi_open = false;
  bool audio_open = false;
  int midi_port_index = -1;
  std::vector<cardlink::midi::MidiPortInfo> midi_ports;
  std::vector<std::string> serial_ports;
  int serial_port_index = 0;
  char serial_path_buf[256] = {};
  uint32_t baud = 460800;
  int gain_db = 6;
  OutMode out_mode = OutMode::Card;
  GuiView view = GuiView::Perform;

  char raw_cmd[256] = {};
  int raw_target = 0; // 0 channel, 1 effect, 2 all (console target tabs)
  std::deque<std::string> lab_history;
  int lab_history_index = -1;
  char log_filter[64] = {};
  bool show_shortcuts = false;
  bool show_about = false;

  char attack_cdc_path[256] = {};
  int attack_cdc_port_index = 0;

  float ui_scale = 1.f; // user zoom (Cmd/Ctrl +/-/0), persisted
  bool log_collapsed = false;
  float layout_log_w = 288.f; // right LOG + CONSOLE width (design 288)
  bool auto_reconnect = false;
  bool settings_dirty = false;
  float settings_save_countdown = 0.f;

  int shape_mode = 0;
  float shape_param = 0.5f;
  float filter_hz_f = 5000.f;
  float filter_q_f = 1.f;
  float filter_k_f = 0.f;
  bool filter_bypass = true;
  int filter_voice = 0; // n0..n7 chip selection on the Filter panel
  bool env_apply_all = false;
  int selected_voice = 0;
  int piano_octave = 0; // offset from C4 (0 = C4–C5)
  int piano_velocity = 100;
  /** Perform scope controls (design: continuous halve/double + slider). */
  float scope_time_ms = 4.f; // ms per division, 0.1–200
  float scope_volt = 1.f;    // amplitude compression, 0.1–8
  float peak_hold = 0.f;
  float peak_hold_timer = 0.f;
  bool midi_activity = false;
  float midi_activity_timer = 0.f;

  /** Status-bar popover state (BUS / MIDI chips). */
  bool bus_popover = false;
  bool midi_popover = false;

  EffectUiState effect;

  /** Named envelope presets stored next to settings. */
  std::vector<std::pair<std::string, EnvProgram>> env_presets;
  std::vector<EnvProgram> env_undo;
  char env_preset_name[64] = "My patch";

  std::deque<LogEntry> log_view;
  std::deque<LogEntry> poll_log_view;
  /** 0 = activity (bus/console), 1 = vq / ring poll. */
  int log_panel_tab = 0;
  bool log_auto_scroll = true;

  std::array<float, cardlink::midi::kVoiceCount> voice_glow{};
  std::vector<std::string> pending_drops;

  void RefreshPortLists();
  /**
   * Refresh serial list and ensure attack_cdc_path is an openable Channel Card
   * CDC (cu.usbmodem*CHCARD* preferred). Updates attack_cdc_path on success.
   */
  bool EnsureAttackCdc(std::string &err);
  bool EnsureSampleUac();
  void Tick();
  void Draw();
  void DrawSidebar();
  void DrawStatusBar();
  void DrawActivityLog();
  void DrawPerform();
  void DrawTone();
  void DrawSetup();
  void DrawAbout();
  void DrawBusFaultBanner();
  void DrawShortcutSheet();
  void DrawDisconnectedHint(const char *action);
  void SendConsole(const char *cmd);
  void ApplyBankEvents(const std::vector<cardlink::midi::BankEvent> &events);

  /** Local preview / VoiceBank only — card already got the console command. */
  void ApplyLocalBankEvents(const std::vector<cardlink::midi::BankEvent> &events);
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
