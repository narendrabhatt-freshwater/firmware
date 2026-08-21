/**
 * @file main.cpp
 * @brief rs485_console — optional PC-side client for the Channel/Effect
 *        RS485 bus. Any serial terminal at 921600 8N1 can speak the same
 *        ASCII protocol; this tool adds targeting, retries, and --echo-off.
 *        See docs/rs485_console_architecture.md.
 */

#include "host_io/rs485/link.hpp"
#include "host_io/serial_port.hpp"
#include "host_io/rs485/types.hpp"

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

using host_io::rs485::ExchangeResult;
using host_io::rs485::Link;
using host_io::rs485::LinkOptions;
using host_io::rs485::ParseTarget;
using host_io::SerialPort;
using host_io::rs485::Status;
using host_io::rs485::Target;
using host_io::rs485::TargetName;

namespace
{

void PrintUsage()
{
  std::cout
      << "rs485_console - optional RS485 client (same ASCII as screen/minicom)\n"
         "\n"
         "Usage:\n"
         "  rs485_console --list\n"
         "  rs485_console --port <path> [options]\n"
         "  rs485_console --port <path> [options] send channel <command>\n"
         "\n"
         "Options:\n"
         "  --port PATH        Serial device for the RS485 adapter\n"
         "  --baud N           Baud rate (default 921600)\n"
         "  --target TARGET    Default: channel|effect|all (default channel)\n"
         "  --timeout-ms N     Per-attempt reply wait, ms (default 500)\n"
         "  --retries N        Extra attempts on timeout (default 2)\n"
         "  --echo-off         Send e:ec 0 at start (burst / half-duplex)\n"
         "  --manual-rts       Toggle RTS around each transmit\n"
         "  --list             List likely serial ports and exit\n"
         "  -h, --help         This help\n"
         "\n"
         "Wire: host lines end with CR only; replies are [C]/[E]…\\r\\n.\n"
         "With device echo off, enable local echo in your terminal.\n"
         "Any serial terminal can type the same commands (e.g. e:ec 0).\n"
         "Channel Card also accepts:\n"
         "  f0..f7 <Hz> / f <Hz>   LPF slots 0..7 (20000=bypass)\n"
         "  g / n 0 / cpu           gain, silence-all, load probe\n"
         "  h                      list Channel commands\n";
}

struct Options
{
  std::string port;
  uint32_t baud = 921600;
  Target target = Target::Channel;
  uint32_t timeout_ms = 500;
  int retries = 2;
  bool manual_rts = false;
  bool echo_off = false;
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
    else if (a == "--echo-off")
    {
      opts.echo_off = true;
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
  return printable * 4 >= s.size() * 3;
}

void PrintHexDump(const std::string &raw)
{
  std::cerr << "(rx noise/garbage, " << raw.size()
            << " byte(s) — not a card reply)\n  hex:";
  for (unsigned char c : raw)
  {
    char buf[8];
    std::snprintf(buf, sizeof(buf), " %02x", c);
    std::cerr << buf;
  }
  std::cerr << "\n";
}

void PrintReply(const ExchangeResult &result)
{
  std::string reply = result.raw;
  if (reply.empty())
    return;
  if (!LooksLikeTextReply(reply))
  {
    PrintHexDump(reply);
    return;
  }
#if defined(_WIN32)
  std::cout << reply << "\n";
#else
  static const bool is_tty = ::isatty(1);
  if (!is_tty)
  {
    std::cout << reply << "\n";
    return;
  }
  if (reply.rfind("[C]", 0) == 0)
    std::cout << "\x1b[36m" << reply << "\x1b[0m\n";
  else if (reply.rfind("[E]", 0) == 0)
    std::cout << "\x1b[33m" << reply << "\x1b[0m\n";
  else
    std::cout << reply << "\n";
#endif
}

void PrintStatus(const ExchangeResult &result)
{
  switch (result.status)
  {
  case Status::Ok:
    PrintReply(result);
    break;
  case Status::Err:
    std::cerr << "(card err:" << result.err_code << ") ";
    PrintReply(result);
    break;
  case Status::Timeout:
    std::cerr << "(timeout — bus busy or no card listening)\n";
    break;
  case Status::BadReply:
    std::cerr << "(bad reply) ";
    PrintReply(result);
    break;
  case Status::IoError:
    std::cerr << "(I/O error) " << result.raw << "\n";
    break;
  case Status::BusBusy:
    std::cerr << "(bus busy)\n";
    break;
  }
}

int RunSendOnce(Link &link, Target target, const std::string &command)
{
  ExchangeResult result = link.Send(target, command);
  PrintStatus(result);
  return result.ok() ? 0 : 1;
}

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

  if (lower.size() >= 2 && lower[0] == 'n')
  {
    char d = lower[1];
    bool hex = (d >= '0' && d <= '9') || (d >= 'a' && d <= 'f');
    if (hex && (lower.size() == 2 || lower[2] == ' '))
      return lower;
  }

  if (lower.rfind("g ", 0) == 0)
    return lower;

  /* f / f0..f7 filter cmds */
  if (lower[0] == 'f' &&
      (lower.size() == 1 || lower[1] == ' ' ||
       ((lower[1] >= '0' && lower[1] <= '7') &&
        (lower.size() == 2 || lower[2] == ' '))))
    return lower;

  if (lower.rfind("n ", 0) == 0 || lower == "n" ||
      lower.rfind("cpu", 0) == 0)
    return lower;

  char *end = nullptr;
  std::strtod(line.c_str(), &end);
  if (end != line.c_str() && end != nullptr && *end == '\0')
    return std::string("n0 ") + line;

  return lower;
}

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
    if (cooked.c_cc[VERASE] == 0 || cooked.c_cc[VERASE] == _POSIX_VDISABLE)
      cooked.c_cc[VERASE] = 0177;
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

int RunRepl(Link &link, Target default_target, const std::string &port,
            uint32_t baud, bool did_echo_off)
{
  TtyCookedGuard tty;

  std::cout << "rs485_console — connected " << port << " @ " << baud
            << " 8N1, target: " << TargetName(default_target) << "\n"
            << "Same protocol as screen/minicom. 'quit' exits.\n";
  if (did_echo_off)
  {
    std::cout << "(e:ec 0 sent — use local terminal echo if typing manually)\n";
  }

  if (default_target == Target::Channel || default_target == Target::All)
  {
    ExchangeResult init = link.Send(Target::Channel, "n0");
    if (init.ok() || init.status == Status::Err)
      PrintStatus(init);
    else
      std::cerr << "(warn: could not apply session defaults)\n";
  }

  std::string line;
  Target target = default_target;
  while (true)
  {
    std::cout << "[" << TargetName(target) << "]> " << std::flush;
    if (!std::getline(std::cin, line))
      break;

    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
      line.pop_back();
    if (line.empty())
      continue;

    if (line == "quit" || line == "exit")
      break;

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

    PrintStatus(link.Send(target, NormalizeN0Command(line)));
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
      std::cout << "(no likely serial ports found)\n";
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
  Link link(port, link_opts);

  bool did_echo_off = false;
  if (opts.echo_off)
  {
    ExchangeResult echo = link.Send(Target::Effect, "ec 0");
    PrintStatus(echo);
    if (echo.ok())
    {
      did_echo_off = true;
    }
    else
    {
      std::cerr << "(warn: e:ec 0 failed — turn echo off manually if needed)\n";
    }
  }

  if (!opts.positional.empty() && opts.positional[0] == "send")
  {
    if (opts.positional.size() < 3)
    {
      std::cerr << "usage: rs485_console --port PATH send <channel|effect|all> "
                   "<command...>\n";
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

  return RunRepl(link, opts.target, opts.port, opts.baud, did_echo_off);
}
