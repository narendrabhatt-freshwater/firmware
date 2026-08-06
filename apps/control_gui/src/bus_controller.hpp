#pragma once

#include "log_buffer.hpp"
#include "voice_bank.hpp"

#include "rs485/session.hpp"
#include "rs485/types.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

/**
 * Owns the RS485 Session. Single-flight worker: note slot updates and
 * arbitrary Exec lines share one queue so half-duplex stays sane.
 */
class BusController
{
public:
  BusController();
  ~BusController();

  BusController(const BusController &) = delete;
  BusController &operator=(const BusController &) = delete;

  /** Open adapter + bootstrap (ec 0, n0, g, n 0). Logs to log. */
  bool Open(const std::string &path,
            uint32_t baud,
            uint32_t atten_db,
            LogBuffer &log);

  void Close(LogBuffer &log);

  bool IsOpen() const { return open_.load(); }
  bool BusFault() const { return halted_.load(); }
  std::string Path() const;

  /** Publish desired Hz from VoiceBank (offs before ons in worker). */
  void PublishBank(const midi_host::VoiceBank &bank);

  /** Queue Silence (n 0). */
  void RequestSilence();

  /** Soft recover + clear halt latch if possible. */
  void RequestRecover(LogBuffer &log);

  /** Queue arbitrary console line (c:/e:/ prefix optional). */
  void QueueExec(rs485::Target target, std::string command);

  /** CH1 gain at next opportunity (also used from Transport slider). */
  void QueueGain(uint8_t atten_db);

  uint32_t AttenDb() const { return atten_db_.load(); }
  uint32_t TimeoutCount() const;
  uint32_t ErrCount() const;

private:
  enum class JobKind : uint8_t
  {
    Notes = 0,
    Exec,
    Gain,
    Silence,
    Recover,
  };

  struct Job
  {
    JobKind kind = JobKind::Notes;
    rs485::Target target = rs485::Target::Channel;
    std::string command;
    uint8_t atten_db = 0;
  };

  struct Impl;
  std::unique_ptr<Impl> impl_;

  std::atomic<bool> open_{false};
  std::atomic<bool> halted_{false};
  std::atomic<uint32_t> atten_db_{6};

  void WorkerMain(LogBuffer *log);
};
