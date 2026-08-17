#include "cardproto/effect.hpp"

#include <cstdio>

namespace cardproto
{

  EffectClient::EffectClient(IConsoleTransport &transport) : tx_(transport) {}

  Result EffectClient::Send(const std::string &cmd)
  {
    return tx_.Exchange(Target::Effect, cmd);
  }

  Result EffectClient::Help() { return Send("h"); }

  Result EffectClient::Exec(const std::string &command) { return Send(command); }

  Result EffectClient::Status() { return Send("s"); }

  Result EffectClient::Get48V() { return Send("v"); }

  std::string FormatSet48V(bool on) { return on ? "v 1" : "v 0"; }

  Result EffectClient::Set48V(bool on) { return Send(FormatSet48V(on)); }

  std::string FormatSetLedFlash(bool on) { return on ? "l 1" : "l 0"; }

  Result EffectClient::SetLedFlash(bool on)
  {
    return Send(FormatSetLedFlash(on));
  }

  std::string FormatSetLedRed(bool on) { return on ? "lr 1" : "lr 0"; }

  Result EffectClient::SetLedRed(bool on) { return Send(FormatSetLedRed(on)); }

  std::string FormatSetLedYellow(bool on) { return on ? "ly 1" : "ly 0"; }

  Result EffectClient::SetLedYellow(bool on)
  {
    return Send(FormatSetLedYellow(on));
  }

  std::string FormatSetAudioEn(bool on) { return on ? "a 1" : "a 0"; }

  Result EffectClient::SetAudioEn(bool on) { return Send(FormatSetAudioEn(on)); }

  Result EffectClient::I2cScan() { return Send("i2c"); }

  Result EffectClient::AdcInit() { return Send("ai"); }

  std::string FormatAdcRead(uint8_t chip, uint8_t reg)
  {
    char cmd[24];
    std::snprintf(cmd, sizeof(cmd), "ar %u %u", static_cast<unsigned>(chip),
                  static_cast<unsigned>(reg));
    return cmd;
  }

  Result EffectClient::AdcRead(uint8_t chip, uint8_t reg)
  {
    if (chip < 1u || chip > 2u)
    {
      return Result::LocalErr("range", "adc chip");
    }
    return Send(FormatAdcRead(chip, reg));
  }

  std::string FormatAdcWrite(uint8_t chip, uint8_t reg, uint8_t value)
  {
    char cmd[32];
    std::snprintf(cmd, sizeof(cmd), "aw %u %u %u", static_cast<unsigned>(chip),
                  static_cast<unsigned>(reg), static_cast<unsigned>(value));
    return cmd;
  }

  Result EffectClient::AdcWrite(uint8_t chip, uint8_t reg, uint8_t value)
  {
    if (chip < 1u || chip > 2u)
    {
      return Result::LocalErr("range", "adc chip");
    }
    return Send(FormatAdcWrite(chip, reg, value));
  }

  Result EffectClient::GetUsbAdcCh() { return Send("u"); }

  std::string FormatSetUsbAdcCh(uint8_t ch)
  {
    char cmd[16];
    std::snprintf(cmd, sizeof(cmd), "u %u", static_cast<unsigned>(ch));
    return cmd;
  }

  Result EffectClient::SetUsbAdcCh(uint8_t ch)
  {
    if (ch < 1u || ch > 8u)
    {
      return Result::LocalErr("range", "usb adc ch");
    }
    return Send(FormatSetUsbAdcCh(ch));
  }

  Result EffectClient::GetEcho() { return Send("ec"); }

  std::string FormatSetEcho(bool on) { return on ? "ec 1" : "ec 0"; }

  Result EffectClient::SetEcho(bool on) { return Send(FormatSetEcho(on)); }

} // namespace cardproto
