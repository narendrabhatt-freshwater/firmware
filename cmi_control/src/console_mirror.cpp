#include "console_mirror.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace
{

std::string ToLower(std::string s)
{
  for (char &c : s) {
    c = static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

/** Strip optional c:/e:/ *: wire prefix. */
std::string StripPrefix(std::string cmd)
{
  if (cmd.size() >= 2 && cmd[1] == ':') {
    const char id = cmd[0];
    if (id == 'c' || id == 'e' || id == '*') {
      cmd.erase(0, 2);
    }
  }
  while (!cmd.empty() &&
         std::isspace(static_cast<unsigned char>(cmd.front()))) {
    cmd.erase(cmd.begin());
  }
  return cmd;
}

bool ParseBool01(const char *tok, bool *out)
{
  if (!tok || !out) {
    return false;
  }
  if (std::strcmp(tok, "0") == 0) {
    *out = false;
    return true;
  }
  if (std::strcmp(tok, "1") == 0) {
    *out = true;
    return true;
  }
  return false;
}

void MirrorChannel(const std::string &cmd, const cardproto::Result &result,
                   UiMirrorPatch &p)
{
  (void)result;

  /* Bare n0 = session defaults (gain + bypass). n0 <hz> is a note set. */
  if (cmd == "n0") {
    p.has_gain_db = true;
    p.gain_db = 0;
    p.has_filter_bypass = true;
    p.filter_bypass = true;
    return;
  }

  /* n <hz>|0 — all SAMPLE voices */
  if (cmd.size() >= 2 && cmd[0] == 'n' && cmd[1] == ' ') {
    double hz = 0.0;
    if (std::sscanf(cmd.c_str() + 2, "%lf", &hz) == 1) {
      if (hz == 0.0 || (hz >= 20.0 && hz < 20000.0)) {
        p.has_note = true;
        p.note_slot = -1;
        p.note_hz = hz;
      }
    }
    return;
  }

  /* n0..n7 <hz> [scale] — single SAMPLE voice */
  if (cmd.size() >= 2 && cmd[0] == 'n') {
    int slot = -1;
    if (cmd[1] >= '0' && cmd[1] <= '9') {
      slot = cmd[1] - '0';
    } else if (cmd[1] >= 'a' && cmd[1] <= 'f') {
      slot = 10 + (cmd[1] - 'a');
    }
    if (slot >= 0 && slot <= 7 && (cmd.size() == 2 || cmd[2] == ' ')) {
      if (cmd.size() == 2) {
        /* bare n1..n7 = syntax err on card; ignore */
        return;
      }
      double hz = 0.0;
      if (std::sscanf(cmd.c_str() + 3, "%lf", &hz) == 1) {
        if (hz == 0.0 || (hz >= 20.0 && hz < 20000.0)) {
          p.has_note = true;
          p.note_slot = slot;
          p.note_hz = hz;
        }
      }
      return;
    }
  }

  unsigned ch = 0;
  unsigned db = 0;
  if (std::sscanf(cmd.c_str(), "g %u %u", &ch, &db) == 2 && ch == 1u &&
      db <= 127u) {
    p.has_gain_db = true;
    p.gain_db = static_cast<int>(db);
    return;
  }

  if (cmd == "s") {
    p.has_shape = true;
    p.shape_mode = 0;
    p.shape_param = 0.5f;
    return;
  }

  double param = 0.0;
  if (std::sscanf(cmd.c_str(), "p %lf", &param) == 1 && param >= 0.1 &&
      param <= 0.9) {
    p.has_shape = true;
    p.shape_mode = 1;
    p.shape_param = static_cast<float>(param);
    return;
  }
  if (std::sscanf(cmd.c_str(), "t %lf", &param) == 1 && param >= 0.1 &&
      param <= 0.9) {
    p.has_shape = true;
    p.shape_mode = 2;
    p.shape_param = static_cast<float>(param);
    return;
  }

  /* fk / fk0..fk7 — before bare f */
  double kd = 0.0;
  unsigned fslot = 0;
  if (cmd.rfind("fk", 0) == 0) {
    if (cmd.size() >= 3 && cmd[2] >= '0' && cmd[2] <= '7' &&
        (cmd.size() == 3 || cmd[3] == ' ')) {
      fslot = static_cast<unsigned>(cmd[2] - '0');
      const char *rest = cmd.c_str() + 3;
      while (*rest == ' ') {
        ++rest;
      }
      if (*rest != '\0' && std::sscanf(rest, "%lf", &kd) == 1 && kd >= 0.0 &&
          kd <= 10.0) {
        p.has_filter_k = true;
        p.filter_k_f = static_cast<float>(kd);
        p.has_filter_voice = true;
        p.filter_voice = static_cast<int>(fslot);
      }
      return;
    }
    if (cmd == "fk") {
      return; /* query — no UI dump parse */
    }
    if (cmd.size() > 3 && cmd[2] == ' ' &&
        std::sscanf(cmd.c_str() + 3, "%lf", &kd) == 1 && kd >= 0.0 &&
        kd <= 10.0) {
      p.has_filter_k = true;
      p.filter_k_f = static_cast<float>(kd);
    }
    return;
  }

  /* f / f0..f7 */
  if (!cmd.empty() && cmd[0] == 'f') {
    double hz = 0.0;
    double q = 0.0;
    if (cmd.size() >= 2 && cmd[1] >= '0' && cmd[1] <= '7' &&
        (cmd.size() == 2 || cmd[2] == ' ')) {
      fslot = static_cast<unsigned>(cmd[1] - '0');
      const char *rest = cmd.c_str() + 2;
      while (*rest == ' ') {
        ++rest;
      }
      if (*rest == '\0') {
        return; /* query */
      }
      const int n = std::sscanf(rest, "%lf %lf", &hz, &q);
      if (n < 1) {
        return;
      }
      p.has_filter_voice = true;
      p.filter_voice = static_cast<int>(fslot);
      p.has_filter_hz = true;
      p.filter_hz_f = static_cast<float>(hz);
      p.has_filter_bypass = true;
      p.filter_bypass = (hz == 0.0 || hz >= 20000.0);
      if (n >= 2 && q >= 0.5 && q <= 10.0) {
        p.has_filter_q = true;
        p.filter_q_f = static_cast<float>(q);
      }
      return;
    }
    if (cmd == "f") {
      return; /* query dump */
    }
    if (cmd.size() > 2 && cmd[1] == ' ') {
      const int n = std::sscanf(cmd.c_str() + 2, "%lf %lf", &hz, &q);
      if (n < 1) {
        return;
      }
      p.has_filter_hz = true;
      p.filter_hz_f = static_cast<float>(hz);
      p.has_filter_bypass = true;
      p.filter_bypass = (hz == 0.0 || hz >= 20000.0);
      if (n >= 2 && q >= 0.5 && q <= 10.0) {
        p.has_filter_q = true;
        p.filter_q_f = static_cast<float>(q);
      }
    }
    return;
  }

  /* w0..w7 <rate> */
  if (cmd.size() >= 2 && cmd[0] == 'w' && cmd[1] >= '0' && cmd[1] <= '7') {
    const int slot = cmd[1] - '0';
    if (cmd.size() == 2) {
      return; /* query */
    }
    if (cmd[2] != ' ') {
      return;
    }
    double rate = 0.0;
    if (std::sscanf(cmd.c_str() + 3, "%lf", &rate) != 1) {
      return;
    }
    if (rate > 0.0 && rate <= 192000.0) {
      p.has_wave = true;
      p.wave_slot = slot;
      p.wave_rate = static_cast<float>(rate);
    }
  }
}

void MirrorEffect(const std::string &cmd, UiMirrorPatch &p)
{
  bool on = false;
  if (cmd.rfind("v ", 0) == 0 && ParseBool01(cmd.c_str() + 2, &on)) {
    p.has_fx_phantom = true;
    p.fx_phantom = on;
    return;
  }
  if (cmd.rfind("a ", 0) == 0 && ParseBool01(cmd.c_str() + 2, &on)) {
    p.has_fx_audio_en = true;
    p.fx_audio_en = on;
    return;
  }
  if (cmd.rfind("ec ", 0) == 0 && ParseBool01(cmd.c_str() + 3, &on)) {
    p.has_fx_echo = true;
    p.fx_echo = on;
    return;
  }
  if (cmd.rfind("lr ", 0) == 0 && ParseBool01(cmd.c_str() + 3, &on)) {
    p.has_fx_led_red = true;
    p.fx_led_red = on;
    return;
  }
  if (cmd.rfind("ly ", 0) == 0 && ParseBool01(cmd.c_str() + 3, &on)) {
    p.has_fx_led_yellow = true;
    p.fx_led_yellow = on;
    return;
  }
  if (cmd.rfind("l ", 0) == 0 && ParseBool01(cmd.c_str() + 2, &on)) {
    p.has_fx_led_flash = true;
    p.fx_led_flash = on;
    return;
  }
  unsigned uch = 0;
  if (std::sscanf(cmd.c_str(), "u %u", &uch) == 1 && uch >= 1u && uch <= 8u) {
    p.has_fx_usb_adc_ch = true;
    p.fx_usb_adc_ch = static_cast<int>(uch);
  }
}

} // namespace

bool UiMirrorPatch::Any() const
{
  return has_gain_db || has_shape || has_filter_hz ||
         has_filter_q || has_filter_k || has_filter_bypass ||
         has_filter_voice || has_wave || has_fx_phantom || has_fx_audio_en ||
         has_fx_echo || has_fx_led_flash || has_fx_led_red ||
         has_fx_led_yellow || has_fx_usb_adc_ch || has_note;
}

UiMirrorPatch ParseConsoleMirror(cardproto::Target target,
                                 const std::string &command,
                                 const cardproto::Result &result)
{
  UiMirrorPatch p;
  if (!result.ok()) {
    return p;
  }

  std::string cmd = StripPrefix(ToLower(command));
  while (!cmd.empty() &&
         std::isspace(static_cast<unsigned char>(cmd.back()))) {
    cmd.pop_back();
  }
  if (cmd.empty()) {
    return p;
  }

  const bool channel =
      (target == cardproto::Target::Channel || target == cardproto::Target::All);
  const bool effect =
      (target == cardproto::Target::Effect || target == cardproto::Target::All);

  /* Bare `s`: Channel sine only — never treat as Effect status on All. */
  if (cmd == "s") {
    if (target == cardproto::Target::Channel) {
      MirrorChannel(cmd, result, p);
    }
    return p;
  }

  if (channel) {
    MirrorChannel(cmd, result, p);
    if (p.Any()) {
      return p;
    }
  }
  if (effect) {
    MirrorEffect(cmd, p);
  }
  return p;
}
