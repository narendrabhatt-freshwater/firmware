/**
 * @file main.cpp
 * @brief rs485_console — standalone PC-side console for the Channel/Effect
 *        Card RS485 bus. See docs/rs485_console_architecture.md §3.
 *
 * Talks to either/both cards purely over RS485 through a PC-side adapter
 * (ADAM-4520 or a generic USB-RS485 dongle) — no dependency on either
 * card's own USB CDC connection.
 */

#include "rs485_link.hpp"
#include "serial_port.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

using rs485::ExchangeResult;
using rs485::LinkOptions;
using rs485::ParseTarget;
using rs485::RS485Link;
using rs485::SerialPort;
using rs485::Target;
using rs485::TargetName;

namespace
{

  void PrintUsage()
  {
    std::cout << "rs485_console - Channel Card N0..NF note bank over RS485\n"
                 "\n"
                 "Usage:\n"
                 "  rs485_console --list\n"
                 "  rs485_console --port <path> [options]\n"
                 "  rs485_console --port <path> [options] send channel <nX-command>\n"
                 "\n"
                 "Options:\n"
                 "  --port PATH        Serial device for the RS485 adapter (required unless --list)\n"
                 "  --baud N            Baud rate (default 115200)\n"
                 "  --target TARGET     Default target: channel|effect|all (default channel)\n"
                 "  --timeout-ms N      Per-attempt reply wait, ms (default 500)\n"
                 "  --retries N         Extra send attempts if no reply arrives (default 2)\n"
                 "  --manual-rts        Toggle RTS around each transmit (rare dongles only)\n"
                 "  --list              List likely serial ports and exit\n"
                 "  -h, --help          This help\n"
                 "\n"
                 "Channel Card commands (RS485 / USB CDC):\n"
                 "  N0                      bypass on + gain 1 0 (session defaults)\n"
                 "  N0..NF <Hz> [scale]     note 0..15; scale 0..1 optional (default 1.0)\n"
                 "  e.g. N0 440 0.5 / N1 550 / N2 660 0.1\n"
                 "  gain <ch> <dB>          DAC atten 0..127 on CH1..4 (e.g. gain 1 40)\n"
                 "\n"
                 "Entering the REPL sends bare 'n0' once (bypass on, gain 1 0).\n"
                 "Bare numbers are sent as 'n0 <Hz>'. 'quit' exits.\n";
  }

  struct Options
  {
    std::string port;
    uint32_t baud = 115200;
    Target target = Target::Channel;
    uint32_t timeout_ms = 500;
    int retries = 2;
    bool manual_rts = false;
    bool list = false;
    bool help = false;
    std::vector<std::string> positional;
  };

  bool ParseArgs(int argc, char **argv, Options &opts, std::string &err)
  {
    for (int i = 1; i < argc; i++)
    {
      std::string a = argv[i];
      auto need_value = [&](const char *name) -> const char *
      {
        if (i + 1 >= argc)
        {
          err = std::string(name) + " needs a value";
          return nullptr;
        }
        return argv[++i];
      };

      if (a == "--port")
      {
        const char *v = need_value("--port");
        if (!v)
          return false;
        opts.port = v;
      }
      else if (a == "--baud")
      {
        const char *v = need_value("--baud");
        if (!v)
          return false;
        opts.baud = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
      }
      else if (a == "--target")
      {
        const char *v = need_value("--target");
        if (!v)
          return false;
        if (!ParseTarget(v, opts.target))
        {
          err = std::string("unknown --target '") + v + "' (use channel|effect|all)";
          return false;
        }
      }
      else if (a == "--timeout-ms")
      {
        const char *v = need_value("--timeout-ms");
        if (!v)
          return false;
        opts.timeout_ms = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
      }
      else if (a == "--retries")
      {
        const char *v = need_value("--retries");
        if (!v)
          return false;
        opts.retries = std::atoi(v);
      }
      else if (a == "--manual-rts")
      {
        opts.manual_rts = true;
      }
      else if (a == "--list")
      {
        opts.list = true;
      }
      else if (a == "-h" || a == "--help")
      {
        opts.help = true;
      }
      else if (!a.empty() && a[0] == '-')
      {
        err = "unknown option: " + a;
        return false;
      }
      else
      {
        opts.positional.push_back(a);
      }
    }
    return true;
  }

  /** True if `s` is mostly console text (printable ASCII + CR/LF/TAB).
   * A single high-bit framing/noise byte must not be treated as a real
   * card reply — those print as the Unicode replacement glyph and hide
   * the real problem. */
  bool LooksLikeTextReply(const std::string &s)
  {
    if (s.empty())
      return false;
    size_t printable = 0;
    for (unsigned char c : s)
    {
      if (c == '\r' || c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7f))
        printable++;
    }
    return printable * 4 >= s.size() * 3; /* >= 75% printable */
  }

  void PrintHexDump(const std::string &raw)
  {
    std::cerr << "(rx noise/garbage, " << raw.size()
              << " byte(s) — not a card reply; check A/B polarity, GND, "
                 "termination)\n  hex:";
    for (unsigned char c : raw)
    {
      char buf[8];
      std::snprintf(buf, sizeof(buf), " %02x", c);
      std::cerr << buf;
    }
    std::cerr << "\n";
  }

  /** Prints a reply, colorizing the [C]/[E] card tag when stdout is a TTY.
   * Non-text bytes (bus noise) get a hex dump on stderr instead of a
   * replacement glyph. */
  void PrintReply(const std::string &reply)
  {
    if (!LooksLikeTextReply(reply))
    {
      PrintHexDump(reply);
      return;
    }
#if defined(_WIN32)
    std::cout << reply;
#else
    static const bool is_tty = ::isatty(1);
    if (!is_tty)
    {
      std::cout << reply;
      return;
    }
    std::istringstream iss(reply);
    std::string line;
    while (std::getline(iss, line))
    {
      if (line.rfind("[C]", 0) == 0)
      {
        std::cout << "\x1b[36m" << line << "\x1b[0m\n"; /* cyan */
      }
      else if (line.rfind("[E]", 0) == 0)
      {
        std::cout << "\x1b[33m" << line << "\x1b[0m\n"; /* yellow */
      }
      else
      {
        std::cout << line << "\n";
      }
    }
#endif
  }

  int RunSendOnce(RS485Link &link, Target target, const std::string &command)
  {
    ExchangeResult result = link.Send(target, command);
    if (!result.got_reply)
    {
      std::cerr << "(no reply — bus busy or no card listening; "
                   "check wiring/target/port)\n";
      return 1;
    }
    PrintReply(result.reply);
    return 0;
  }

  /** Map REPL / one-shot input to a Channel Card note/gain command.
   * "1000.5" / "0" → "n0 1000.5" / "n0 0"; "n0"…"nf" / "N0"…"NF" left as-is
   * (link lowercases before TX). */
  std::string NormalizeN0Command(std::string line)
  {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
      line.erase(line.begin());
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    if (line.empty())
      return line;

    std::string lower = line;
    for (char &c : lower)
    {
      if (c >= 'A' && c <= 'Z')
        c = static_cast<char>(c - 'A' + 'a');
    }

    /* n0..nf (optional args) pass through. */
    if (lower.size() >= 2 && lower[0] == 'n')
    {
      char d = lower[1];
      bool hex = (d >= '0' && d <= '9') || (d >= 'a' && d <= 'f');
      if (hex && (lower.size() == 2 || lower[2] == ' '))
        return lower;
    }

    if (lower.rfind("gain ", 0) == 0)
      return lower;

    /* Bare number → wrap as n0 <Hz>. */
    char *end = nullptr;
    std::strtod(line.c_str(), &end);
    if (end != line.c_str() && end != nullptr && *end == '\0')
      return std::string("n0 ") + line;

    return lower;
  }

  /** Put the keyboard TTY into cooked line-edit mode for the REPL (Enter
   * submits, Backspace deletes) and restore whatever it was on exit.
   *
   * getline() relies on the OS line discipline — if something left the
   * session in raw mode (or echo-without-ICANON), Enter/Backspace print as
   * literal ^M/^H instead of submitting/erasing. The RS485 serial port's
   * own termios is separate; this only touches stdin when it is a TTY. */
  class TtyCookedGuard
  {
  public:
    TtyCookedGuard()
    {
#if defined(_WIN32)
      HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
      if (h == INVALID_HANDLE_VALUE || h == nullptr)
        return;
      if (!GetConsoleMode(h, &saved_))
        return;
      handle_ = h;
      DWORD mode = saved_ | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                   ENABLE_PROCESSED_INPUT;
      SetConsoleMode(handle_, mode);
      active_ = true;
#else
      if (!isatty(STDIN_FILENO))
        return;
      if (tcgetattr(STDIN_FILENO, &saved_) != 0)
        return;
      struct termios cooked = saved_;
      cooked.c_lflag |= static_cast<tcflag_t>(ICANON | ECHO | ECHOE | ECHOK | ISIG);
      cooked.c_iflag |= static_cast<tcflag_t>(ICRNL);
      cooked.c_oflag |= static_cast<tcflag_t>(OPOST);
      /* Prefer the usual erase/kill so Backspace deletes instead of
       * printing ^H — leave VERASE alone if the session already set one. */
      if (cooked.c_cc[VERASE] == 0 || cooked.c_cc[VERASE] == _POSIX_VDISABLE)
        cooked.c_cc[VERASE] = 0177; /* DEL — common macOS/Terminal default */
      if (tcsetattr(STDIN_FILENO, TCSANOW, &cooked) == 0)
        active_ = true;
#endif
    }

    ~TtyCookedGuard()
    {
      if (!active_)
        return;
#if defined(_WIN32)
      SetConsoleMode(handle_, saved_);
#else
      tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
#endif
    }

    TtyCookedGuard(const TtyCookedGuard &) = delete;
    TtyCookedGuard &operator=(const TtyCookedGuard &) = delete;

  private:
    bool active_ = false;
#if defined(_WIN32)
    HANDLE handle_ = nullptr;
    DWORD saved_ = 0;
#else
    struct termios saved_{};
#endif
  };

  int RunRepl(RS485Link &link, Target default_target, const std::string &port,
              uint32_t baud)
  {
    TtyCookedGuard tty; /* Enter submits, Backspace deletes — see class note */

    std::cout << "rs485_console — connected " << port << " @ " << baud
              << " 8N1, target: " << TargetName(default_target) << "\n"
              << "Channel Card: n0..nf <Hz> [scale] | gain <ch> <dB>  (enter applies bypass + gain 1 0)\n"
              << "type a frequency, 'n0 440 0.5', 'n1 550', or 'gain 1 40'; 'quit' to exit\n";

    /* Entering the console applies session defaults on the Channel Card. */
    if (default_target == Target::Channel || default_target == Target::All)
    {
      ExchangeResult init = link.Send(Target::Channel, "n0");
      if (init.got_reply)
      {
        PrintReply(init.reply);
      }
      else
      {
        std::cerr << "(warn: could not apply session defaults — is channel on the bus?)\n";
      }
    }

    std::string line;
    Target target = default_target;
    while (true)
    {
      std::cout << "[" << TargetName(target) << "]> " << std::flush;
      if (!std::getline(std::cin, line))
        break;

      /* Trim trailing \r (in case stdin is piped from a CRLF source). */
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
      if (line.empty())
        continue;

      if (line == "quit" || line == "exit")
      {
        break;
      }
      if (line.rfind("card ", 0) == 0)
      {
        Target new_target;
        if (ParseTarget(line.substr(5), new_target))
        {
          target = new_target;
          std::cout << "(default target -> " << TargetName(target) << ")\n";
        }
        else
        {
          std::cerr << "err: card <channel|effect|all>\n";
        }
        continue;
      }

      std::string cmd = NormalizeN0Command(line);
      ExchangeResult result = link.Send(target, cmd);
      if (!result.got_reply)
      {
        std::cerr << "(no reply — bus busy or no card listening)\n";
      }
      else
      {
        PrintReply(result.reply);
      }
    }
    return 0;
  }

} // namespace

int main(int argc, char **argv)
{
  Options opts;
  std::string err;
  if (!ParseArgs(argc, argv, opts, err))
  {
    std::cerr << "error: " << err << "\n\n";
    PrintUsage();
    return 2;
  }
  if (opts.help)
  {
    PrintUsage();
    return 0;
  }

  if (opts.list)
  {
    std::vector<std::string> ports = SerialPort::ListPorts();
    if (ports.empty())
    {
      std::cout << "(no likely serial ports found)\n";
    }
    else
    {
      for (const auto &p : ports)
        std::cout << p << "\n";
    }
    return 0;
  }

  if (opts.port.empty())
  {
    std::cerr << "error: --port is required (or use --list)\n\n";
    PrintUsage();
    return 2;
  }

  SerialPort port;
  port.SetManualRtsControl(opts.manual_rts);
  if (!port.Open(opts.port, opts.baud))
  {
    std::cerr << "error: could not open " << opts.port << ": "
              << port.LastError() << "\n";
    return 1;
  }

  LinkOptions link_opts;
  link_opts.reply_timeout_ms = opts.timeout_ms;
  link_opts.retries = opts.retries;
  RS485Link link(port, link_opts);

  /* `send <target> <command...>` one-shot mode. */
  if (!opts.positional.empty() && opts.positional[0] == "send")
  {
    if (opts.positional.size() < 3)
    {
      std::cerr << "usage: rs485_console --port PATH send <channel|effect|all> <command...>\n";
      return 2;
    }
    Target target;
    if (!ParseTarget(opts.positional[1], target))
    {
      std::cerr << "unknown target '" << opts.positional[1]
                << "' (use channel|effect|all)\n";
      return 2;
    }
    std::string command;
    for (size_t i = 2; i < opts.positional.size(); i++)
    {
      if (i > 2)
        command += " ";
      command += opts.positional[i];
    }
    return RunSendOnce(link, target, NormalizeN0Command(command));
  }

  return RunRepl(link, opts.target, opts.port, opts.baud);
}
