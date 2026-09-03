#pragma once

#include "cardproto/result.hpp"
#include "cardproto/types.hpp"

#include <cstdint>
#include <string>

/**
 * Last-sent UI fields inferred from a successful Log+Console Exec.
 * Only fields with has_* set should be applied.
 */
struct UiMirrorPatch
{
  bool has_gain_db = false;
  int gain_db = 0;

  bool has_filter_hz = false;
  float filter_hz_f = 5000.f;

  bool has_filter_q = false;
  float filter_q_f = 1.f;

  bool has_filter_k = false;
  float filter_k_f = 0.f;

  bool has_filter_bypass = false;
  bool filter_bypass = true;

  bool has_filter_voice = false;
  int filter_voice = 0;

  bool has_fx_phantom = false;
  bool fx_phantom = false;

  bool has_fx_audio_en = false;
  bool fx_audio_en = false;

  bool has_fx_echo = false;
  bool fx_echo = false;

  bool has_fx_led_flash = false;
  bool fx_led_flash = false;

  bool has_fx_led_red = false;
  bool fx_led_red = false;

  bool has_fx_led_yellow = false;
  bool fx_led_yellow = false;

  bool has_fx_usb_adc_ch = false;
  int fx_usb_adc_ch = 1;

  /** Console note bank: slot -1 means all-off. */
  bool has_note = false;
  int note_slot = -1;
  bool note_on = false;
  uint8_t note_key = 0u;
  uint8_t note_velocity = 127u;

  bool Any() const;
};

/**
 * Parse a successful console exchange into a UI patch.
 * Uses TX args for sets.
 * @return Empty patch (Any()==false) when the command does not map to UI.
 */
UiMirrorPatch ParseConsoleMirror(cardproto::Target target,
                                 const std::string &command,
                                 const cardproto::Result &result);
