/**
 * @file effect.hpp
 * @brief Typed Effect Card console API (console protocol specification §3).
 *
 * @par EffectClient
 * Validates where the wire defines ranges, then Exchange()s with
 * Target::Effect. Out-of-range args return Result::LocalErr without TX.
 *
 * @par Format*
 * Pure encoders only — no validation, no trailing CR.
 */

#ifndef PROTOCOL_EFFECT_HPP
#define PROTOCOL_EFFECT_HPP

#include "protocol/result.hpp"
#include "protocol/transport.hpp"

#include <cstdint>
#include <string>

namespace protocol {

/**
 * @brief High-level Effect Card client over an injected transport.
 *
 * All Exchange calls use Target::Effect. The transport may still prepend
 * `e:` on a shared RS485 bus.
 */
class EffectClient {
public:
  /**
   * @brief Construct a client bound to @p transport.
   * @param[in] transport Host pipe (not owned; must outlive this client).
   */
  explicit EffectClient(IConsoleTransport &transport);

  /**
   * @brief Request the one-line help menu.
   * @return Exchange result for wire `h`.
   */
  Result Help();

  /**
   * @brief Send a raw Effect command string (still Target::Effect).
   * @param[in] command ASCII tokens without target prefix or trailing CR.
   * @return Transport exchange result (no local validation of @p command).
   */
  Result Exec(const std::string &command);

  /**
   * @brief Query Effect status summary.
   * @return Exchange result for wire `s`.
   */
  Result Status();

  /**
   * @brief Query 48 V phantom state.
   * @return Exchange result for wire `v`.
   */
  Result Get48V();

  /**
   * @brief Enable or disable 48 V phantom power.
   * @param[in] on True → `v 1`, false → `v 0`.
   * @return Exchange result.
   */
  Result Set48V(bool on);

  /**
   * @brief Enable or disable the flash LED pattern.
   * @param[in] on True → `l 1`, false → `l 0`.
   * @return Exchange result.
   */
  Result SetLedFlash(bool on);

  /**
   * @brief Drive the red LED.
   * @param[in] on True → `lr 1`, false → `lr 0`.
   * @return Exchange result.
   */
  Result SetLedRed(bool on);

  /**
   * @brief Drive the yellow LED.
   * @param[in] on True → `ly 1`, false → `ly 0`.
   * @return Exchange result.
   */
  Result SetLedYellow(bool on);

  /**
   * @brief Enable or disable the audio path.
   * @param[in] on True → `a 1`, false → `a 0`.
   * @return Exchange result.
   */
  Result SetAudioEn(bool on);

  /**
   * @brief Scan the Effect I2C bus.
   * @return Exchange result for wire `i2c` (may be multi-line on the card).
   */
  Result I2cScan();

  /**
   * @brief Initialize ADC path.
   * @return Exchange result for wire `ai`.
   */
  Result AdcInit();

  /**
   * @brief Read one ADC register.
   * @param[in] chip ADC index `1` or `2`.
   * @param[in] reg  Register address in `[0, 255]`.
   * @return LocalErr on bad @p chip; otherwise wire `ar …` exchange result.
   */
  Result AdcRead(uint8_t chip, uint8_t reg);

  /**
   * @brief Write one ADC register.
   * @param[in] chip  ADC index `1` or `2`.
   * @param[in] reg   Register address in `[0, 255]`.
   * @param[in] value Byte to write.
   * @return LocalErr on bad @p chip; otherwise wire `aw …` exchange result.
   */
  Result AdcWrite(uint8_t chip, uint8_t reg, uint8_t value);

  /**
   * @brief Query which ADC channel is routed to USB.
   * @return Exchange result for wire `u`.
   */
  Result GetUsbAdcCh();

  /**
   * @brief Select USB ADC channel.
   * @param[in] ch Channel index in `[1, 8]`.
   * @return LocalErr on bad @p ch; otherwise wire `u …` exchange result.
   */
  Result SetUsbAdcCh(uint8_t ch);

  /**
   * @brief Query RS485 keystroke echo state.
   * @return Exchange result for wire `ec`.
   */
  Result GetEcho();

  /**
   * @brief Enable or disable RS485 keystroke echo.
   *
   * Keep echo off on a shared bus used by MIDI / automation hosts — echo
   * traffic races with tagged console replies.
   *
   * @param[in] on True → `ec 1`, false → `ec 0`.
   * @return Exchange result.
   */
  Result SetEcho(bool on);

private:
  IConsoleTransport &tx_;
  Result Send(const std::string &cmd);
};

/**
 * @name Format helpers (Effect)
 * @brief Encode wire tokens only — no validation, no trailing CR.
 * @{
 */

/**
 * @param[in] on Phantom enable.
 * @return `"v 1"` or `"v 0"`.
 */
std::string FormatSet48V(bool on);

/**
 * @param[in] on Flash LED enable.
 * @return `"l 1"` or `"l 0"`.
 */
std::string FormatSetLedFlash(bool on);

/**
 * @param[in] on Red LED state.
 * @return `"lr 1"` or `"lr 0"`.
 */
std::string FormatSetLedRed(bool on);

/**
 * @param[in] on Yellow LED state.
 * @return `"ly 1"` or `"ly 0"`.
 */
std::string FormatSetLedYellow(bool on);

/**
 * @param[in] on Audio path enable.
 * @return `"a 1"` or `"a 0"`.
 */
std::string FormatSetAudioEn(bool on);

/**
 * @param[in] chip ADC index.
 * @param[in] reg  Register address.
 * @return Command string for wire `ar …`.
 */
std::string FormatAdcRead(uint8_t chip, uint8_t reg);

/**
 * @param[in] chip  ADC index.
 * @param[in] reg   Register address.
 * @param[in] value Byte value.
 * @return Command string for wire `aw …`.
 */
std::string FormatAdcWrite(uint8_t chip, uint8_t reg, uint8_t value);

/**
 * @param[in] ch USB ADC channel 1…8.
 * @return Command string for wire `u …`.
 */
std::string FormatSetUsbAdcCh(uint8_t ch);

/**
 * @param[in] on Echo enable.
 * @return `"ec 1"` or `"ec 0"`.
 */
std::string FormatSetEcho(bool on);

/** @} */

} // namespace protocol

#endif /* PROTOCOL_EFFECT_HPP */
