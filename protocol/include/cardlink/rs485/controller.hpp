#pragma once

#include "cardlink/midi/voice_bank.hpp"
#include "cardproto/channel.hpp"
#include "cardproto/effect.hpp"
#include "cardproto/result.hpp"
#include "cardproto/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace cardlink::rs485
{

enum class QueueResult : uint8_t
{
  Ok = 0,
  Closed,
  Halted,
  Connecting,
};

/**
 * Threaded, single-flight RS485 controller for interactive host applications.
 *
 * Notes and typed/raw commands share one worker so half-duplex ordering is
 * preserved. Callbacks run on the controller's open or worker thread.
 */
class Controller
{
public:
  using ChannelOp = std::function<cardproto::Result(cardproto::ChannelClient &)>;
  using EffectOp = std::function<cardproto::Result(cardproto::EffectClient &)>;
  using LogHandler = std::function<void(const std::string &)>;
  using CommandHandler =
      std::function<void(cardproto::Target, const std::string &,
                         const cardproto::Result &)>;
  /** Called when vq reports a voice idle after note activity. */
  using IdleHandler = std::function<void(uint8_t slot)>;
  /** Exact BODY refill credit plus last applied USB PACK sequence. */
  using VqHandler = std::function<void(uint8_t, uint8_t,
                                       const std::array<uint16_t, 8> &,
                                       uint16_t)>;

  Controller();
  ~Controller();

  Controller(const Controller &) = delete;
  Controller &operator=(const Controller &) = delete;

  /** Open adapter and run the standard Bus bootstrap. */
  bool Open(const std::string &path, uint32_t baud, uint32_t atten_db);
  /** Non-blocking open; IsConnecting() remains true until it finishes. */
  void RequestOpen(const std::string &path, uint32_t baud, uint32_t atten_db);
  void Close();

  bool IsOpen() const;
  bool IsConnecting() const;
  bool BusFault() const;
  std::string Path() const;

  /** Publish desired frequencies; note-offs are sent before note-ons. */
  void PublishBank(const cardlink::midi::VoiceBank &bank);
  void RequestSilence();
  void RequestRecover();

  QueueResult QueueExec(cardproto::Target target, std::string command);
  QueueResult QueueChannel(ChannelOp op);
  QueueResult QueueEffect(EffectOp op);
  QueueResult QueueGain(uint8_t atten_db);

  uint32_t AttenDb() const;
  uint32_t TimeoutCount() const;
  uint32_t ErrCount() const;
  std::size_t QueueDepth() const;

  void AcknowledgeSlotKey(uint8_t slot, uint8_t key);
  void AcknowledgeSlotOff(uint8_t slot);

  void SetLogHandler(LogHandler handler);
  /** Status-poll messages use this callback, or the normal log when unset. */
  void SetPollLogHandler(LogHandler handler);
  /** Called after each successful raw QueueExec command. */
  void SetCommandHandler(CommandHandler handler);
  void SetIdleHandler(IdleHandler handler);
  /** Installing a handler enables continuous sequential vq polling. */
  void SetVqHandler(VqHandler handler);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace cardlink::rs485
