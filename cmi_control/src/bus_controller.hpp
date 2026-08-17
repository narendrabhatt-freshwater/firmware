#pragma once

#include "console_mirror.hpp"
#include "log_buffer.hpp"
#include "cardlink/rs485/controller.hpp"
#include "cardlink/midi/voice_bank.hpp"
#include "cardproto/channel.hpp"
#include "cardproto/effect.hpp"
#include "cardproto/result.hpp"
#include "cardproto/types.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using BusQueueResult = cardlink::rs485::QueueResult;

/**
 * UI adapter for cardlink::rs485::Controller logging and console mirroring.
 */
class BusController
{
public:
  using ChannelOp = cardlink::rs485::Controller::ChannelOp;
  using EffectOp = cardlink::rs485::Controller::EffectOp;
  using IdleHandler = cardlink::rs485::Controller::IdleHandler;
  using VqHandler = cardlink::rs485::Controller::VqHandler;

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

  bool IsOpen() const;
  bool IsConnecting() const;
  bool BusFault() const;
  std::string Path() const;

  /** Publish desired Hz from VoiceBank (offs before ons in worker). */
  void PublishBank(const cardlink::midi::VoiceBank &bank);

  /** Queue Silence (n 0). */
  void RequestSilence();

  /**
   * Optional: Silence UAC dry when card finishes release (vq idle).
   * Thread-safe; may be invoked from the bus worker.
   */
  void SetIdleHandler(IdleHandler handler);
  void SetVqHandler(VqHandler handler);

  /** Soft recover + clear halt latch if possible. */
  void RequestRecover(LogBuffer &log);

  /**
   * Optional buffer for vq / ring-fill chatter (timeout, underrun, bad reply).
   * Kept out of the main activity log. Not owned.
   */
  void SetPollLog(LogBuffer *poll_log);

  /** Queue arbitrary console line (c:/e:/ prefix optional). */
  BusQueueResult QueueExec(cardproto::Target target, std::string command);

  /** Typed Channel / Effect ops run on the worker thread. */
  BusQueueResult QueueChannel(ChannelOp op);
  BusQueueResult QueueEffect(EffectOp op);

  /** CH1 gain at next opportunity (also used from Transport slider). */
  BusQueueResult QueueGain(uint8_t atten_db);

  uint32_t AttenDb() const;
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
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
