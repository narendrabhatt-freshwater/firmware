#pragma once

#include "log_buffer.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class WaveSlotUi : uint8_t
{
  Empty = 0,
  Assigned,
  Queued,
  Uploading,
  Done,
  Failed,
};

struct WaveSlotState
{
  WaveSlotUi ui = WaveSlotUi::Empty;
  char path[512] = {};
  char status[96] = {};
  float progress = 0.f; // 0..1 for active upload
  static constexpr int kPreviewN = 128;
  float preview[kPreviewN] = {};
  bool has_preview = false;
};

/**
 * Serial background uploader for w0..w7. UI thread only mutates paths /
 * queues jobs; worker owns CDC I/O. Poll Snapshot() each frame.
 */
class WaveUploadQueue
{
public:
  WaveUploadQueue();
  ~WaveUploadQueue();

  WaveUploadQueue(const WaveUploadQueue &) = delete;
  WaveUploadQueue &operator=(const WaveUploadQueue &) = delete;

  void SetCdcPath(std::string path);
  std::string CdcPath() const;

  void SetSlotPath(int slot, const std::string &path);
  void ClearSlot(int slot);

  /** Queue all Assigned slots that have a path (or only `only_slot` if >= 0). */
  bool StartUpload(LogBuffer &log, int only_slot = -1);
  void Cancel();

  bool Busy() const { return busy_.load(); }
  float OverallProgress() const { return overall_.load(); }
  int CurrentSlot() const { return current_slot_.load(); }

  void Snapshot(std::array<WaveSlotState, 8> &out) const;

private:
  void Worker();
  void PushJob(int slot);

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::thread thr_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> busy_{false};
  std::atomic<bool> cancel_{false};
  std::atomic<float> overall_{0.f};
  std::atomic<int> current_slot_{-1};

  std::string cdc_path_;
  std::array<WaveSlotState, 8> slots_{};
  std::vector<int> queue_;
  LogBuffer *log_ = nullptr;
};
