#include "settings.hpp"

#include "theme.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#endif

namespace fw::settings
{
namespace
{

std::string ConfigDir()
{
#if defined(_WIN32)
  char buf[MAX_PATH] = {};
  if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
    return std::string(buf) + "\\CMI";
  }
  return ".";
#elif defined(__APPLE__)
  const char *home = std::getenv("HOME");
  if (home && home[0]) {
    return std::string(home) + "/Library/Application Support/CMI";
  }
  return ".";
#else
  const char *xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && xdg[0]) {
    return std::string(xdg) + "/cmi";
  }
  const char *home = std::getenv("HOME");
  if (home && home[0]) {
    return std::string(home) + "/.config/cmi";
  }
  return ".";
#endif
}

/** Pre-CMI branding path — read once if the new config is absent. */
std::string LegacyConfigDir()
{
#if defined(_WIN32)
  char buf[MAX_PATH] = {};
  if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
    return std::string(buf) + "\\Freshwater";
  }
  return ".";
#elif defined(__APPLE__)
  const char *home = std::getenv("HOME");
  if (home && home[0]) {
    return std::string(home) + "/Library/Application Support/Freshwater";
  }
  return ".";
#else
  const char *xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && xdg[0]) {
    return std::string(xdg) + "/freshwater";
  }
  const char *home = std::getenv("HOME");
  if (home && home[0]) {
    return std::string(home) + "/.config/freshwater";
  }
  return ".";
#endif
}

void EnsureDir(const std::string &dir)
{
#if defined(_WIN32)
  CreateDirectoryA(dir.c_str(), nullptr);
#else
  std::string cmd = "mkdir -p \"" + dir + "\"";
  (void)std::system(cmd.c_str());
#endif
}

void WriteKV(std::ofstream &out, const char *key, const std::string &val)
{
  out << key << '=' << val << '\n';
}

void WriteKV(std::ofstream &out, const char *key, int v)
{
  out << key << '=' << v << '\n';
}

void WriteKV(std::ofstream &out, const char *key, float v)
{
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(v));
  out << key << '=' << buf << '\n';
}

void WriteKV(std::ofstream &out, const char *key, bool v)
{
  out << key << '=' << (v ? 1 : 0) << '\n';
}

bool ParseLine(const std::string &line, std::string &key, std::string &val)
{
  if (line.empty() || line[0] == '#' || line[0] == '[') {
    return false;
  }
  const auto eq = line.find('=');
  if (eq == std::string::npos) {
    return false;
  }
  key = line.substr(0, eq);
  val = line.substr(eq + 1);
  while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
    key.pop_back();
  }
  return !key.empty();
}

} // namespace

std::string Path() { return ConfigDir() + "/control_gui.ini"; }

bool Load(App &app)
{
  std::ifstream in(Path());
  if (!in) {
    /* One-shot migration from the pre-CMI config directory. */
    const std::string legacy = LegacyConfigDir() + "/control_gui.ini";
    in.open(legacy);
    if (!in) {
      return false;
    }
  }
  std::string line;
  std::string key;
  std::string val;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!ParseLine(line, key, val)) {
      continue;
    }
    if (key == "view") {
      app.view = static_cast<GuiView>(std::clamp(std::atoi(val.c_str()), 0, 4));
    } else if (key == "nav_expanded") {
      app.nav_expanded = (std::atoi(val.c_str()) != 0);
    } else if (key == "nav_width") {
      app.nav_width = std::strtof(val.c_str(), nullptr);
    } else if (key == "log_collapsed") {
      app.log_collapsed = (std::atoi(val.c_str()) != 0);
    } else if (key == "layout_log_h") {
      app.layout_log_h = std::strtof(val.c_str(), nullptr);
    } else if (key == "layout_perform_voice_h") {
      app.layout_perform_voice_h = std::strtof(val.c_str(), nullptr);
    } else if (key == "layout_perform_piano_h") {
      app.layout_perform_piano_h = std::strtof(val.c_str(), nullptr);
    } else if (key == "layout_tone_left_w") {
      app.layout_tone_left_w = std::strtof(val.c_str(), nullptr);
    } else if (key == "gain_db") {
      app.gain_db = std::clamp(std::atoi(val.c_str()), 0, 127);
    } else if (key == "out_mode") {
      app.out_mode =
          static_cast<OutMode>(std::clamp(std::atoi(val.c_str()), 0, 2));
    } else if (key == "baud") {
      app.baud = static_cast<uint32_t>(std::atoi(val.c_str()));
    } else if (key == "serial_path") {
      std::snprintf(app.serial_path_buf, sizeof(app.serial_path_buf), "%s",
                    val.c_str());
    } else if (key == "wave_cdc_path") {
      std::snprintf(app.wave_cdc_path, sizeof(app.wave_cdc_path), "%s",
                    val.c_str());
    } else if (key == "midi_port_index") {
      app.midi_port_index = std::atoi(val.c_str());
    } else if (key == "auto_reconnect") {
      app.auto_reconnect = (std::atoi(val.c_str()) != 0);
    } else if (key == "play_mode") {
      app.play_mode = std::clamp(std::atoi(val.c_str()), 0, 1);
    } else if (key == "piano_octave") {
      app.piano_octave = std::clamp(std::atoi(val.c_str()), -4, 3);
    } else if (key == "ui_scale") {
      app.ui_scale =
          std::clamp(std::strtof(val.c_str(), nullptr), 0.75f, 1.75f);
    }
  }
  /* Shared CMI Control.ini is reused across branches. A leftover 460800
   * from the previous playground session opens the FTDI port but the
   * card is 921600, so handshake RX is empty. */
  app.baud = 921600;
  return true;
}

bool Save(const App &app)
{
  const std::string dir = ConfigDir();
  EnsureDir(dir);
  std::ofstream out(Path(), std::ios::trunc);
  if (!out) {
    return false;
  }
  out << "# CMI Control settings\n";
  WriteKV(out, "view", static_cast<int>(app.view));
  WriteKV(out, "nav_expanded", app.nav_expanded);
  WriteKV(out, "nav_width", app.nav_width);
  WriteKV(out, "log_collapsed", app.log_collapsed);
  WriteKV(out, "layout_log_h", app.layout_log_h);
  WriteKV(out, "layout_perform_voice_h", app.layout_perform_voice_h);
  WriteKV(out, "layout_perform_piano_h", app.layout_perform_piano_h);
  WriteKV(out, "layout_tone_left_w", app.layout_tone_left_w);
  WriteKV(out, "gain_db", app.gain_db);
  WriteKV(out, "out_mode", static_cast<int>(app.out_mode));
  WriteKV(out, "baud", static_cast<int>(app.baud));
  WriteKV(out, "serial_path", std::string(app.serial_path_buf));
  WriteKV(out, "wave_cdc_path", std::string(app.wave_cdc_path));
  WriteKV(out, "midi_port_index", app.midi_port_index);
  WriteKV(out, "auto_reconnect", app.auto_reconnect);
  WriteKV(out, "play_mode", app.play_mode);
  WriteKV(out, "piano_octave", app.piano_octave);
  WriteKV(out, "ui_scale", app.ui_scale);
  return true;
}

} // namespace fw::settings
