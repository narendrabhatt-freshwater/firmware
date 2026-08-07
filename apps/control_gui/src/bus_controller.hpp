#pragma once

#include "log_buffer.hpp"
#include "voice_bank.hpp"

#include "host_io/rs485/bus.hpp"
#include "protocol/channel.hpp"
#include "protocol/effect.hpp"
#include "protocol/result.hpp"

#include "protocol/types.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

/**
 * Owns the RS485 bus. Single-flight worker: note slot updates and
 * typed / raw console ops share one queue so half-duplex stays sane.
 */
class BusController
{
public:
  using ChannelOp = std::function<protocol::Result(protocol::ChannelClient &)>;
  using EffectOp = std::function<protocol::Result(protocol::EffectClient &)>;

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
  void QueueExec(protocol::Target target, std::string command);

  /** Typed Channel / Effect ops run on the worker thread. */
  void QueueChannel(ChannelOp op);
  void QueueEffect(EffectOp op);

  void QueueMode(protocol::PlayMode mode);
  void QueuePlayWave(uint8_t slot, double rate_hz);
  void QueueStopWave(uint8_t slot);

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
    Channel,
    Effect,
  };

  struct Job
  {
    JobKind kind = JobKind::Notes;
    protocol::Target target = protocol::Target::Channel;
    std::string command;
    uint8_t atten_db = 0;
    ChannelOp channel_op;
    EffectOp effect_op;
  };

  struct Impl;
  std::unique_ptr<Impl> impl_;

  std::atomic<bool> open_{false};
  std::atomic<bool> halted_{false};
  std::atomic<uint32_t> atten_db_{6};

  void WorkerMain(LogBuffer *log);
  void Enqueue(Job job);
};
