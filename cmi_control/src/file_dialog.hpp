#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

/** Non-blocking native file/folder pickers (macOS/Linux). Poll ResultReady(). */
class AsyncFileDialog
{
public:
  AsyncFileDialog() = default;
  ~AsyncFileDialog();

  AsyncFileDialog(const AsyncFileDialog &) = delete;
  AsyncFileDialog &operator=(const AsyncFileDialog &) = delete;

  bool Busy() const { return busy_.load(); }
  bool BeginPickFile();
  bool BeginPickFolder();

  /** If a pick finished, moves the path into `out` and returns true. */
  bool TakeResult(std::string &out);

private:
  void Join();

  std::atomic<bool> busy_{false};
  std::mutex mu_;
  std::string result_;
  bool ready_ = false;
  std::thread worker_;
};
