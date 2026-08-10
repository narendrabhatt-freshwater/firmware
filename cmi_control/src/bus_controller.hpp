#pragma once

#include "console_mirror.hpp"
#include "log_buffer.hpp"
#include "voice_bank.hpp"

#include "cardlink/rs485/bus.hpp"
#include "cardproto/channel.hpp"
#include "cardproto/effect.hpp"
#include "cardproto/result.hpp"

#include "cardproto/types.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

enum class BusQueueResult : uint8_t
{
  Ok = 0,
  Closed,
  Halted,
  Connecting,
};

/**
 * Owns the RS485 bus. Single-flight worker: note slot updates and
 * typed / raw console ops share one queue so half-duplex stays sane.
 */
class BusController
{
public:
  using ChannelOp = std::function<cardproto::Result(cardproto::ChannelClient &)>;
  using EffectOp = std::function<cardproto::Result(cardproto::EffectClient &)>;
  /** Called when vq reports a voice idle after note activity (stop UAC dry). */
  using IdleHandler = std::function<void(uint8_t slot)>;

  BusController();
  ~BusController();

  BusController(const BusController &) = delete;
  BusController &operator=(const BusController &) = delete;

  /** Open adapter + bootstrap (ec 0, n0, g, n 0). Logs to log. */
  bool Open(const std::string &path,
            uint32_t baud,
            uint32_t atten_db,
            LogBuffer &log);

  /** Non-blocking open on a background thread; UI polls IsConnecting(). */
  void RequestOpen(const std::string &path,
                   uint32_t baud,
                   uint32_t atten_db,
                   LogBuffer &log);

  void Close(LogBuffer &log);

  bool IsOpen() const { return open_.load(); }
  bool IsConnecting() const { return connecting_.load(); }
  bool BusFault() const { return halted_.load(); }
  std::string Path() const;

  /** Publish desired Hz from VoiceBank (offs before ons in worker). */
  void PublishBank(const midi_host::VoiceBank &bank);

  /** Queue Silence (n 0). */
  void RequestSilence();

  /**
   * Optional: Silence UAC dry when card finishes release (vq idle).
   * Thread-safe; may be invoked from the bus worker.
   */
  void SetIdleHandler(IdleHandler handler);

  /** Soft recover + clear halt latch if possible. */
  void RequestRecover(LogBuffer &log);

  /**
   * Optional buffer for vq / ring-fill chatter (underrun, bad reply).
   * Kept out of the main activity log. Not owned.
   */
  void SetPollLog(LogBuffer *poll_log);

  /** Queue arbitrary console line (c:/e:/ prefix optional). */
  BusQueueResult QueueExec(cardproto::Target target, std::string command);

  /** Typed Channel / Effect ops run on the worker thread. */
  BusQueueResult QueueChannel(ChannelOp op);
  BusQueueResult QueueEffect(EffectOp op);

  BusQueueResult QueuePlayWave(uint8_t slot, double rate_hz);
  BusQueueResult QueueStopWave(uint8_t slot);

  /** CH1 gain at next opportunity (also used from Transport slider). */
  BusQueueResult QueueGain(uint8_t atten_db);

  uint32_t AttenDb() const { return atten_db_.load(); }
  uint32_t TimeoutCount() const;
  uint32_t ErrCount() const;
  std::size_t QueueDepth() const;

  /** Drain last-sent UI patches from successful Log+Console Exec (UI thread). */
  std::vector<UiMirrorPatch> DrainUiMirror();

  /**
   * Align worker desired/sent Hz with a note already applied on the card
   * (console mirror) so a later PublishBank does not fight or drop it.
   */
  void AcknowledgeSlotHz(uint8_t slot, double hz);
  void AcknowledgeAllHz(double hz);

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
    cardproto::Target target = cardproto::Target::Channel;
    std::string command;
    uint8_t atten_db = 0;
    ChannelOp channel_op;
    EffectOp effect_op;
  };

  struct Impl;
  std::unique_ptr<Impl> impl_;

  std::atomic<bool> open_{false};
  std::atomic<bool> connecting_{false};
  std::atomic<bool> halted_{false};
  std::atomic<uint32_t> atten_db_{6};
  LogBuffer *poll_log_ = nullptr;

  void WorkerMain(LogBuffer *log);
  BusQueueResult Enqueue(Job job);
};
