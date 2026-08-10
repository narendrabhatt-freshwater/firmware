#include "wave_upload_queue.hpp"

#include "wave_cdc.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace
{

void BuildPreview(WaveSlotState &s)
{
  s.has_preview = false;
  std::fill(std::begin(s.preview), std::end(s.preview), 0.f);
  if (s.path[0] == '\0') {
    return;
  }
  std::ifstream in(s.path, std::ios::binary);
  if (!in) {
    return;
  }
  std::vector<char> buf((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
  if (buf.size() < 4 || (buf.size() & 1u) != 0u) {
    return;
  }
  const int16_t *samples = reinterpret_cast<const int16_t *>(buf.data());
  const size_t n = buf.size() / 2;
  const int pn = WaveSlotState::kPreviewN;
  for (int i = 0; i < pn; ++i) {
    const size_t i0 = (static_cast<size_t>(i) * n) / static_cast<size_t>(pn);
    size_t i1 = ((static_cast<size_t>(i) + 1u) * n) / static_cast<size_t>(pn);
    if (i1 <= i0) {
      i1 = i0 + 1;
    }
    if (i1 > n) {
      i1 = n;
    }
    float min_v = 1.f;
    float max_v = -1.f;
    for (size_t j = i0; j < i1; ++j) {
      const float v = static_cast<float>(samples[j]) / 32768.f;
      min_v = std::min(min_v, v);
      max_v = std::max(max_v, v);
    }
    /* Midpoint of min/max keeps peaks visible when downsampling. */
    s.preview[i] = 0.5f * (min_v + max_v);
  }
  s.has_preview = true;
}

} // namespace

WaveUploadQueue::WaveUploadQueue()
{
  thr_ = std::thread([this] { Worker(); });
}

WaveUploadQueue::~WaveUploadQueue()
{
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
    cancel_ = true;
    cv_.notify_all();
  }
  if (thr_.joinable()) {
    thr_.join();
  }
}

void WaveUploadQueue::SetCdcPath(std::string path)
{
  std::lock_guard<std::mutex> lock(mu_);
  cdc_path_ = std::move(path);
}

std::string WaveUploadQueue::CdcPath() const
{
  std::lock_guard<std::mutex> lock(mu_);
  return cdc_path_;
}

void WaveUploadQueue::SetSlotPath(int slot, const std::string &path)
{
  if (slot < 0 || slot > 7) {
    return;
  }
  WaveSlotState built{};
  std::snprintf(built.path, sizeof(built.path), "%s", path.c_str());
  built.ui = path.empty() ? WaveSlotUi::Empty : WaveSlotUi::Assigned;
  built.progress = 0.f;
  if (path.empty()) {
    built.status[0] = '\0';
  } else {
    std::snprintf(built.status, sizeof(built.status), "ready");
    BuildPreview(built);
  }

  std::lock_guard<std::mutex> lock(mu_);
  slots_[static_cast<size_t>(slot)] = built;
}

void WaveUploadQueue::ClearSlot(int slot)
{
  SetSlotPath(slot, {});
}

bool WaveUploadQueue::StartUpload(LogBuffer &log, int only_slot)
{
  std::lock_guard<std::mutex> lock(mu_);
  if (busy_.load()) {
    log.Push("err: upload already running");
    return false;
  }
  if (cdc_path_.empty()) {
    log.Push("err: set Channel USB CDC path first");
    return false;
  }

  queue_.clear();
  cancel_ = false;
  if (only_slot >= 0 && only_slot <= 7) {
    auto &s = slots_[static_cast<size_t>(only_slot)];
    if (s.path[0] == '\0') {
      log.Push("err: slot has no file");
      return false;
    }
    s.ui = WaveSlotUi::Queued;
    std::snprintf(s.status, sizeof(s.status), "queued");
    s.progress = 0.f;
    queue_.push_back(only_slot);
  } else {
    for (int i = 0; i < 8; ++i) {
      auto &s = slots_[static_cast<size_t>(i)];
      if (s.path[0] == '\0') {
        continue;
      }
      s.ui = WaveSlotUi::Queued;
      std::snprintf(s.status, sizeof(s.status), "queued");
      s.progress = 0.f;
      queue_.push_back(i);
    }
  }
  if (queue_.empty()) {
    log.Push("err: no wave files assigned");
    return false;
  }

  log_ = &log;
  busy_ = true;
  overall_ = 0.f;
  cv_.notify_all();
  return true;
}

void WaveUploadQueue::Cancel()
{
  cancel_ = true;
}

void WaveUploadQueue::Snapshot(std::array<WaveSlotState, 8> &out) const
{
  std::lock_guard<std::mutex> lock(mu_);
  out = slots_;
}

void WaveUploadQueue::Worker()
{
  for (;;) {
    std::vector<int> jobs;
    std::string cdc;
    LogBuffer *log = nullptr;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] {
        return stop_.load() || (!queue_.empty() && busy_.load());
      });
      if (stop_.load()) {
        return;
      }
      jobs.swap(queue_);
      cdc = cdc_path_;
      log = log_;
    }

    const int total = static_cast<int>(jobs.size());
    int done = 0;

    WaveCdcSession session;
    if (!session.Open(cdc, *log)) {
      std::lock_guard<std::mutex> lock(mu_);
      for (int s : jobs) {
        auto &st = slots_[static_cast<size_t>(s)];
        st.ui = WaveSlotUi::Failed;
        std::snprintf(st.status, sizeof(st.status), "no cdc");
      }
      busy_ = false;
      current_slot_ = -1;
      continue;
    }

    for (int slot : jobs) {
      if (cancel_.load()) {
        std::lock_guard<std::mutex> lock(mu_);
        for (int s : jobs) {
          auto &st = slots_[static_cast<size_t>(s)];
          if (st.ui == WaveSlotUi::Queued || st.ui == WaveSlotUi::Uploading) {
            st.ui = WaveSlotUi::Assigned;
            std::snprintf(st.status, sizeof(st.status), "cancelled");
            st.progress = 0.f;
          }
        }
        break;
      }

      current_slot_ = slot;
      std::string path;
      {
        std::lock_guard<std::mutex> lock(mu_);
        auto &st = slots_[static_cast<size_t>(slot)];
        st.ui = WaveSlotUi::Uploading;
        std::snprintf(st.status, sizeof(st.status), "uploading…");
        st.progress = 0.f;
        path = st.path;
      }

      const bool ok = session.LoadFile(
          slot, path, *log, [this, slot, total, done](float p) {
            std::lock_guard<std::mutex> lock(mu_);
            slots_[static_cast<size_t>(slot)].progress = p;
            const float base =
                static_cast<float>(done) / static_cast<float>(total);
            overall_ = base + p / static_cast<float>(total);
          });

      {
        std::lock_guard<std::mutex> lock(mu_);
        auto &st = slots_[static_cast<size_t>(slot)];
        if (ok) {
          st.ui = WaveSlotUi::Done;
          st.progress = 1.f;
          std::snprintf(st.status, sizeof(st.status), "on card");
        } else {
          st.ui = WaveSlotUi::Failed;
          st.progress = 0.f;
          std::snprintf(st.status, sizeof(st.status), "failed");
        }
      }
      ++done;
      overall_ = static_cast<float>(done) / static_cast<float>(total);
    }

    session.Close();
    current_slot_ = -1;
    busy_ = false;
    if (!cancel_.load() && log) {
      log->Push("ok: wave bank upload finished");
    }
    cancel_ = false;
  }
}
