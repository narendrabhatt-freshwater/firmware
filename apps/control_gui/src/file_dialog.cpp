#include "file_dialog.hpp"

#include "wave_cdc.hpp"

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
    const std::string path = WaveCdc_PickRawFile();
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
    const std::string path = WaveCdc_PickFolder();
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
