#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

/** Thread-safe ring of bus reply / status lines for the UI log. */
class LogBuffer
{
public:
  static constexpr std::size_t kMaxLines = 400;

  void Push(std::string line)
  {
    std::lock_guard<std::mutex> lock(mu_);
    lines_.push_back(std::move(line));
    while (lines_.size() > kMaxLines) {
      lines_.pop_front();
    }
  }

  /** Copy snapshot for ImGui (call from UI thread). */
  void Snapshot(std::deque<std::string> &out) const
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
  std::deque<std::string> lines_;
};
