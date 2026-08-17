#include "file_dialog.hpp"

#include <cstdio>

namespace
{

#if defined(__APPLE__) || defined(__linux__)
std::string ShellTrim(std::string out)
{
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
    out.pop_back();
  }
  return out;
}

std::string RunPicker(const char *command)
{
  FILE *pipe = popen(command, "r");
  if (pipe == nullptr) {
    return {};
  }
  char buf[1024];
  std::string out;
  while (fgets(buf, sizeof(buf), pipe) != nullptr) {
    out += buf;
  }
  pclose(pipe);
  return ShellTrim(std::move(out));
}
#endif

std::string PickSampleFile()
{
#if defined(__APPLE__)
  return RunPicker(
      "osascript -e 'POSIX path of (choose file with prompt \"Sample file\")' "
      "2>/dev/null");
#elif defined(__linux__)
  return RunPicker(
      "zenity --file-selection --title='Sample file' 2>/dev/null");
#else
  return {};
#endif
}

std::string PickSampleFolder()
{
#if defined(__APPLE__)
  return RunPicker(
      "osascript -e 'POSIX path of (choose folder with prompt \"Sample "
      "folder\")' 2>/dev/null");
#elif defined(__linux__)
  return RunPicker(
      "zenity --file-selection --directory --title='Sample folder' "
      "2>/dev/null");
#else
  return {};
#endif
}

} // namespace

AsyncFileDialog::~AsyncFileDialog() { Join(); }

void AsyncFileDialog::Join()
{
  if (worker_.joinable()) {
    worker_.join();
  }
}

bool AsyncFileDialog::BeginPickFile()
{
  if (busy_.exchange(true)) {
    return false;
  }
  Join();
  {
    std::lock_guard<std::mutex> lock(mu_);
    ready_ = false;
    result_.clear();
  }
  worker_ = std::thread([this] {
    const std::string path = PickSampleFile();
    {
      std::lock_guard<std::mutex> lock(mu_);
      result_ = path;
      ready_ = true;
    }
    busy_.store(false);
  });
  return true;
}

bool AsyncFileDialog::BeginPickFolder()
{
  if (busy_.exchange(true)) {
    return false;
  }
  Join();
  {
    std::lock_guard<std::mutex> lock(mu_);
    ready_ = false;
    result_.clear();
  }
  worker_ = std::thread([this] {
    const std::string path = PickSampleFolder();
    {
      std::lock_guard<std::mutex> lock(mu_);
      result_ = path;
      ready_ = true;
    }
    busy_.store(false);
  });
  return true;
}

bool AsyncFileDialog::TakeResult(std::string &out)
{
  std::lock_guard<std::mutex> lock(mu_);
  if (!ready_) {
    return false;
  }
  out = std::move(result_);
  result_.clear();
  ready_ = false;
  return true;
}
