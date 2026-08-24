/**
 * Sequential RS485 ping-pong: send 1, wait reply 1, send 2, wait reply 2.
 *
 * Times the host-visible command/ACK cycle with no extra idle pad. Channel
 * `vq` on current card firmware is a 26-byte binary frame (A5 5A … LF);
 * other commands still use tagged ASCII (`[C] …\\r\\n`).
 */

#include "cardlink/serial_port.hpp"
#include "cardlink/rs485/types.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

namespace {

constexpr uint8_t kVqSync0 = 0xA5;
constexpr uint8_t kVqSync1 = 0x5A;
constexpr std::size_t kVqFrameLen = cardlink::rs485::kVqBinaryFrameLen;

const char *StatusOk = "ok";
const char *StatusTimeout = "timeout";
const char *StatusBad = "bad";

uint32_t Percentile(std::vector<uint32_t> sorted, double p) {
  if (sorted.empty()) {
    return 0;
  }
  if (sorted.size() == 1) {
    return sorted.front();
  }
  const double idx = static_cast<double>(sorted.size() - 1) * p;
  const size_t lo = static_cast<size_t>(idx);
  const size_t hi = std::min(lo + 1, sorted.size() - 1);
  const double frac = idx - static_cast<double>(lo);
  return static_cast<uint32_t>(sorted[lo] * (1.0 - frac) + sorted[hi] * frac);
}

std::string HexPreview(const uint8_t *p, size_t n) {
  std::string out;
  out.reserve(n * 3);
  for (size_t i = 0; i < n; ++i) {
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%s%02x", i ? " " : "", p[i]);
    out += buf;
  }
  return out;
}

bool LooksLikeVqCmd(const std::string &cmd) {
  return cmd == "vq" || cmd == "c:vq";
}

void Usage() {
  std::fprintf(stderr,
               "usage: cardlink_rs485_rtt [--port PATH] [--baud N] [--cmd vq]\n"
               "                          [--count N] [--warmup N] [--verbose]\n");
}

} // namespace

int main(int argc, char **argv) {
  std::string port_path = "/dev/cu.usbserial-BG03CSYB";
  uint32_t baud = 921600;
  std::string cmd = "vq";
  int count = 200;
  int warmup = 8;
  bool verbose = false;

  for (int i = 1; i < argc; ++i) {
    const char *a = argv[i];
    auto need = [&](const char *flag) -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", flag);
        std::exit(2);
      }
      return argv[++i];
    };
    if (std::strcmp(a, "--port") == 0) {
      port_path = need(a);
    } else if (std::strcmp(a, "--baud") == 0) {
      baud = static_cast<uint32_t>(std::strtoul(need(a), nullptr, 10));
    } else if (std::strcmp(a, "--cmd") == 0) {
      cmd = need(a);
    } else if (std::strcmp(a, "--count") == 0) {
      count = std::atoi(need(a));
    } else if (std::strcmp(a, "--warmup") == 0) {
      warmup = std::atoi(need(a));
    } else if (std::strcmp(a, "--verbose") == 0) {
      verbose = true;
    } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
      Usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown option: %s\n", a);
      Usage();
      return 2;
    }
  }

  if (count < 1) {
    std::fprintf(stderr, "count must be >= 1\n");
    return 2;
  }

  cardlink::SerialPort port;
  if (!port.Open(port_path, baud)) {
    std::fprintf(stderr, "open %s: %s\n", port_path.c_str(),
                 port.LastError().c_str());
    return 1;
  }

  std::string line = cmd;
  if (!(line.size() >= 2 && line[1] == ':')) {
    line = "c:" + cmd;
  }
  const std::string wire_s = line + "\r";
  const auto *wire = reinterpret_cast<const uint8_t *>(wire_s.data());
  const size_t wire_len = wire_s.size();
  const bool binary_vq = LooksLikeVqCmd(cmd);

  std::printf("port %s  %u 8N1\n", port_path.c_str(), baud);
  std::printf("pattern  send %s\\r  → wait reply  → next\n", line.c_str());
  std::printf("reply    %s\n",
              binary_vq ? "26-byte exact-credit vq (A5 5A … LF)"
                        : "tagged ASCII [C]/[E] line");
  std::printf("warmup %d  timed %d\n\n", warmup, count);

  using clock = std::chrono::steady_clock;

  auto read_reply = [&](uint8_t *acc, size_t acc_cap, size_t &acc_n,
                        uint32_t timeout_ms) -> const char * {
    acc_n = 0;
    const auto deadline =
        clock::now() + std::chrono::milliseconds(timeout_ms);
    while (clock::now() < deadline) {
      auto left_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         deadline - clock::now())
                         .count();
      if (left_ms <= 0) {
        break;
      }
      uint8_t buf[64];
      const uint32_t slice =
          static_cast<uint32_t>(std::min<int64_t>(left_ms, 2));
      const size_t n = port.ReadTimeout(buf, sizeof(buf), slice);
      if (n == 0) {
        continue;
      }
      if (acc_n + n > acc_cap) {
        return StatusBad;
      }
      std::memcpy(acc + acc_n, buf, n);
      acc_n += n;

      if (binary_vq) {
        /* Resync to A5 5A if a leading glitch arrives. */
        size_t start = 0;
        while (start + 1 < acc_n &&
               !(acc[start] == kVqSync0 && acc[start + 1] == kVqSync1)) {
          ++start;
        }
        if (start > 0 && start < acc_n) {
          std::memmove(acc, acc + start, acc_n - start);
          acc_n -= start;
        }
        if (acc_n >= kVqFrameLen && acc[0] == kVqSync0 &&
            acc[1] == kVqSync1 && acc[kVqFrameLen - 1] == '\n') {
          acc_n = kVqFrameLen;
          return StatusOk;
        }
      } else {
        std::string s(reinterpret_cast<char *>(acc), acc_n);
        const size_t tag_c = s.find("[C]");
        const size_t tag_e = s.find("[E]");
        size_t tag = std::string::npos;
        if (tag_c != std::string::npos) {
          tag = tag_c;
        }
        if (tag_e != std::string::npos &&
            (tag == std::string::npos || tag_e < tag)) {
          tag = tag_e;
        }
        if (tag == std::string::npos) {
          continue;
        }
        const size_t crlf = s.find("\r\n", tag);
        const size_t lf = s.find('\n', tag);
        if (crlf != std::string::npos || lf != std::string::npos) {
          return StatusOk;
        }
      }
    }
    return StatusTimeout;
  };

  auto once = [&](int n, bool show) -> uint32_t {
    port.FlushInput();
    const auto t0 = clock::now();
    if (!port.Write(wire, wire_len)) {
      std::fprintf(stderr, "#%d write failed: %s\n", n,
                   port.LastError().c_str());
      return 0;
    }
    port.DrainOutput();

    uint8_t acc[96];
    size_t acc_n = 0;
    const char *st = read_reply(acc, sizeof(acc), acc_n, 200);
    const auto us = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(clock::now() -
                                                              t0)
            .count());

    if (show) {
      if (binary_vq) {
        std::printf("#%-3d  %6u us  %-7s  %s\n", n, us, st,
                    HexPreview(acc, acc_n).c_str());
      } else {
        std::string s(reinterpret_cast<char *>(acc), acc_n);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) {
          s.pop_back();
        }
        std::printf("#%-3d  %6u us  %-7s  %s\n", n, us, st, s.c_str());
      }
    }
    return (std::strcmp(st, StatusOk) == 0) ? us : 0;
  };

  for (int i = 1; i <= warmup; ++i) {
    const uint32_t us = once(i, verbose);
    if (us == 0) {
      std::fprintf(stderr, "warmup #%d failed\n", i);
      return 1;
    }
  }

  std::vector<uint32_t> samples;
  samples.reserve(static_cast<size_t>(count));
  int timeouts = 0;
  for (int i = 1; i <= count; ++i) {
    const bool show = verbose || i <= 8 || i == count;
    const uint32_t us = once(i, show);
    if (i == 9 && !verbose) {
      std::printf("  …\n");
    }
    if (us == 0) {
      ++timeouts;
      continue;
    }
    samples.push_back(us);
  }

  if (samples.empty()) {
    std::fprintf(stderr, "no successful replies\n");
    return 1;
  }

  std::vector<uint32_t> ordered = samples;
  std::sort(ordered.begin(), ordered.end());
  const double mean =
      std::accumulate(samples.begin(), samples.end(), 0.0) /
      static_cast<double>(samples.size());
  const uint32_t p50 = Percentile(ordered, 0.50);
  const uint32_t p95 = Percentile(ordered, 0.95);
  const uint32_t p99 = Percentile(ordered, 0.99);
  const double tx_us = (static_cast<double>(wire_len) * 10.0 * 1e6) /
                       static_cast<double>(baud);
  const size_t rx_bytes = binary_vq ? kVqFrameLen : 11;
  const double rx_us =
      (static_cast<double>(rx_bytes) * 10.0 * 1e6) / static_cast<double>(baud);

  std::printf("\nok %zu/%d  timeout %d\n", samples.size(), count, timeouts);
  std::printf("rtt us   min %u  p50 %u  mean %.0f  p95 %u  p99 %u  max %u\n",
              ordered.front(), p50, mean, p95, p99, ordered.back());
  std::printf("wire us  tx %.0f (%zu B)  rx %.0f (~%zu B)  sum %.0f\n", tx_us,
              wire_len, rx_us, rx_bytes, tx_us + rx_us);
  std::printf("max poll  %.0f Hz from mean,  %.0f Hz from p95\n", 1e6 / mean,
              1e6 / static_cast<double>(p95));
  std::printf("poll floor  %.2f ms (p95)   safe ~%.2f ms (p95 + 50%%)\n",
              p95 / 1000.0, (p95 * 1.5) / 1000.0);
  return timeouts == 0 ? 0 : 1;
}
