#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>

/** Log entry classification for the LOG + CONSOLE panel colors. */
enum class LogKind : uint8_t
{
  Info = 0,
  Ok,
  Err,
  Tx,
};

struct LogEntry
{
  char ts[9] = {}; // HH:MM:SS
  LogKind kind = LogKind::Info;
  std::string text;
};

/** Thread-safe ring of bus reply / status lines for the UI log. */
class LogBuffer
{
public:
  static constexpr std::size_t kMaxLines = 400;

  void Push(std::string line)
  {
    // Classify from conventions used across the app / firmware replies.
    LogKind k = LogKind::Info;
    if (line.rfind("ok", 0) == 0 || line.find("ok:") != std::string::npos) {
      k = LogKind::Ok;
    } else if (line.rfind("err", 0) == 0 ||
               line.find("err:") != std::string::npos ||
               line.find("FAULT") != std::string::npos ||
               line.find("fault") != std::string::npos ||
               line.find("Drop") != std::string::npos) {
      k = LogKind::Err;
    } else if (line.rfind("-> ", 0) == 0 || line.rfind("[", 0) == 0) {
      k = LogKind::Tx;
    }
    Push(k, std::move(line));
  }

  void Push(LogKind kind, std::string line)
  {
    LogEntry e;
    e.kind = kind;
    e.text = std::move(line);
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    std::snprintf(e.ts, sizeof(e.ts), "%02d:%02d:%02d", tm_buf.tm_hour,
                  tm_buf.tm_min, tm_buf.tm_sec);
    std::lock_guard<std::mutex> lock(mu_);
    lines_.push_back(std::move(e));
    while (lines_.size() > kMaxLines) {
      lines_.pop_front();
    }
  }

  /** Copy snapshot for ImGui (call from UI thread). */
  void Snapshot(std::deque<LogEntry> &out) const
  {
    std::lock_guard<std::mutex> lock(mu_);
    out = lines_;
  }

  void Clear()
  {
    std::lock_guard<std::mutex> lock(mu_);
    lines_.clear();
  }

private:
  mutable std::mutex mu_;
  std::deque<LogEntry> lines_;
};
