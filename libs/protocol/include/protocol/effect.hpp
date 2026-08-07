/**
 * @file effect.hpp
 * @brief Typed Effect Card console API (console protocol specification §3).
 *
 * EffectClient validates where the wire defines ranges, then Exchange()s
 * with Target::Effect. Format* helpers only encode (no validation, no CR).
 */

#ifndef PROTOCOL_EFFECT_HPP
#define PROTOCOL_EFFECT_HPP

#include "protocol/result.hpp"
#include "protocol/transport.hpp"

#include <cstdint>
#include <string>

namespace protocol {

class EffectClient {
public:
  explicit EffectClient(IConsoleTransport &transport);

  Result Help(); /**< h */
  Result Exec(const std::string &command);

  Result Status();  /**< s */
  Result Get48V();  /**< v */
  Result Set48V(bool on);           /**< v 0|1 */
  Result SetLedFlash(bool on);      /**< l 0|1 */
  Result SetLedRed(bool on);        /**< lr 0|1 */
  Result SetLedYellow(bool on);     /**< ly 0|1 */
  Result SetAudioEn(bool on);       /**< a 0|1 */

  Result I2cScan();                 /**< i2c */
  Result AdcInit();                 /**< ai */
  /** chip 1|2; reg 0…255. */
  Result AdcRead(uint8_t chip, uint8_t reg);
  Result AdcWrite(uint8_t chip, uint8_t reg, uint8_t value);

  Result GetUsbAdcCh();             /**< u */
  Result SetUsbAdcCh(uint8_t ch);   /**< u 1…8 */

  Result GetEcho();                 /**< ec */
  /** Keep off on a shared RS485 bus used by MIDI / automation. */
  Result SetEcho(bool on);

private:
  IConsoleTransport &tx_;
  Result Send(const std::string &cmd);
};

std::string FormatSet48V(bool on);
std::string FormatSetLedFlash(bool on);
std::string FormatSetLedRed(bool on);
std::string FormatSetLedYellow(bool on);
std::string FormatSetAudioEn(bool on);
std::string FormatAdcRead(uint8_t chip, uint8_t reg);
std::string FormatAdcWrite(uint8_t chip, uint8_t reg, uint8_t value);
std::string FormatSetUsbAdcCh(uint8_t ch);
std::string FormatSetEcho(bool on);

} // namespace protocol

#endif /* PROTOCOL_EFFECT_HPP */
