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

#if !defined(_WIN32)
#include <unistd.h>
#endif

using rs485::ExchangeResult;
using rs485::LinkOptions;
using rs485::ParseTarget;
using rs485::RS485Link;
using rs485::SerialPort;
using rs485::Target;
using rs485::TargetName;

namespace {

void PrintUsage() {
  std::cout <<
      "rs485_console - standalone RS485 console for the Channel/Effect Card bus\n"
      "\n"
      "Usage:\n"
      "  rs485_console --list\n"
      "  rs485_console --port <path> [options]\n"
      "  rs485_console --port <path> [options] send <channel|effect|all> <command...>\n"
      "\n"
      "Options:\n"
      "  --port PATH        Serial device for the RS485 adapter (required unless --list)\n"
      "  --baud N            Baud rate (default 115200, matches both cards' UART5/UART4)\n"
      "  --target TARGET     Default target for the REPL: channel|effect|all (default all)\n"
      "  --timeout-ms N      Per-attempt reply wait, ms (default 300)\n"
      "  --retries N         Extra send attempts if no reply arrives (default 2)\n"
      "  --manual-rts        Toggle RTS around each transmit (only for USB-RS485\n"
      "                      dongles without auto-direction; most don't need this)\n"
      "  --list              List likely serial ports for this OS and exit\n"
      "  -h, --help          This help\n"
      "\n"
      "Interactive REPL commands (not sent to the bus):\n"
      "  card channel|effect|all   change the default target\n"
      "  quit / exit                leave the REPL\n"
      "\n"
      "Any other line is sent as a console command, e.g. 'help', 'status',\n"
      "'sw bypass on'. An explicit 'c:'/'e:'/'*:' prefix on a line overrides\n"
      "the current default target for that line only.\n";
}

struct Options {
  std::string port;
  uint32_t baud = 115200;
  Target target = Target::All;
  uint32_t timeout_ms = 300;
  int retries = 2;
  bool manual_rts = false;
  bool list = false;
  bool help = false;
  std::vector<std::string> positional;
};

bool ParseArgs(int argc, char **argv, Options &opts, std::string &err) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto need_value = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        err = std::string(name) + " needs a value";
        return nullptr;
      }
      return argv[++i];
    };

    if (a == "--port") {
      const char *v = need_value("--port");
      if (!v)
        return false;
      opts.port = v;
    } else if (a == "--baud") {
      const char *v = need_value("--baud");
      if (!v)
        return false;
      opts.baud = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    } else if (a == "--target") {
      const char *v = need_value("--target");
      if (!v)
        return false;
      if (!ParseTarget(v, opts.target)) {
        err = std::string("unknown --target '") + v + "' (use channel|effect|all)";
        return false;
      }
    } else if (a == "--timeout-ms") {
      const char *v = need_value("--timeout-ms");
      if (!v)
        return false;
      opts.timeout_ms = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    } else if (a == "--retries") {
      const char *v = need_value("--retries");
      if (!v)
        return false;
      opts.retries = std::atoi(v);
    } else if (a == "--manual-rts") {
      opts.manual_rts = true;
    } else if (a == "--list") {
      opts.list = true;
    } else if (a == "-h" || a == "--help") {
      opts.help = true;
    } else if (!a.empty() && a[0] == '-') {
      err = "unknown option: " + a;
      return false;
    } else {
      opts.positional.push_back(a);
    }
  }
  return true;
}

/** Prints a reply, colorizing the [C]/[E] card tag when stdout is a TTY. */
void PrintReply(const std::string &reply) {
#if defined(_WIN32)
  std::cout << reply;
#else
  static const bool is_tty = ::isatty(1);
  if (!is_tty) {
    std::cout << reply;
    return;
  }
  std::istringstream iss(reply);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.rfind("[C]", 0) == 0) {
      std::cout << "\x1b[36m" << line << "\x1b[0m\n"; /* cyan */
    } else if (line.rfind("[E]", 0) == 0) {
      std::cout << "\x1b[33m" << line << "\x1b[0m\n"; /* yellow */
    } else {
      std::cout << line << "\n";
    }
  }
#endif
}

int RunSendOnce(RS485Link &link, Target target, const std::string &command) {
  ExchangeResult result = link.Send(target, command);
  if (!result.got_reply) {
    std::cerr << "(no reply — bus busy or no card listening; "
                 "check wiring/target/port)\n";
    return 1;
  }
  PrintReply(result.reply);
  return 0;
}

int RunRepl(RS485Link &link, Target default_target, const std::string &port,
            uint32_t baud) {
  std::cout << "rs485_console — connected " << port << " @ " << baud
            << " 8N1, target: " << TargetName(default_target) << "\n"
            << "type a command and press Enter (your terminal echoes as you "
               "type; the firmware doesn't echo over RS485 itself)\n"
            << "'card channel|effect|all' to change target, 'quit' to exit\n";
  std::string line;
  Target target = default_target;
  while (true) {
    std::cout << "[" << TargetName(target) << "]> " << std::flush;
    if (!std::getline(std::cin, line))
      break;

    /* Trim trailing \r (in case stdin is piped from a CRLF source). */
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
      line.pop_back();
    if (line.empty())
      continue;

    if (line == "quit" || line == "exit") {
      break;
    }
    if (line.rfind("card ", 0) == 0) {
      Target new_target;
      if (ParseTarget(line.substr(5), new_target)) {
        target = new_target;
        std::cout << "(default target -> " << TargetName(target) << ")\n";
      } else {
        std::cerr << "err: card <channel|effect|all>\n";
      }
      continue;
    }

    ExchangeResult result = link.Send(target, line);
    if (!result.got_reply) {
      std::cerr << "(no reply — bus busy or no card listening)\n";
    } else {
      PrintReply(result.reply);
    }
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  Options opts;
  std::string err;
  if (!ParseArgs(argc, argv, opts, err)) {
    std::cerr << "error: " << err << "\n\n";
    PrintUsage();
    return 2;
  }
  if (opts.help) {
    PrintUsage();
    return 0;
  }

  if (opts.list) {
    std::vector<std::string> ports = SerialPort::ListPorts();
    if (ports.empty()) {
      std::cout << "(no likely serial ports found)\n";
    } else {
      for (const auto &p : ports)
        std::cout << p << "\n";
    }
    return 0;
  }

  if (opts.port.empty()) {
    std::cerr << "error: --port is required (or use --list)\n\n";
    PrintUsage();
    return 2;
  }

  SerialPort port;
  port.SetManualRtsControl(opts.manual_rts);
  if (!port.Open(opts.port, opts.baud)) {
    std::cerr << "error: could not open " << opts.port << ": "
               << port.LastError() << "\n";
    return 1;
  }

  LinkOptions link_opts;
  link_opts.reply_timeout_ms = opts.timeout_ms;
  link_opts.retries = opts.retries;
  RS485Link link(port, link_opts);

  /* `send <target> <command...>` one-shot mode. */
  if (!opts.positional.empty() && opts.positional[0] == "send") {
    if (opts.positional.size() < 3) {
      std::cerr << "usage: rs485_console --port PATH send <channel|effect|all> <command...>\n";
      return 2;
    }
    Target target;
    if (!ParseTarget(opts.positional[1], target)) {
      std::cerr << "unknown target '" << opts.positional[1]
                << "' (use channel|effect|all)\n";
      return 2;
    }
    std::string command;
    for (size_t i = 2; i < opts.positional.size(); i++) {
      if (i > 2)
        command += " ";
      command += opts.positional[i];
    }
    return RunSendOnce(link, target, command);
  }

  return RunRepl(link, opts.target, opts.port, opts.baud);
}
