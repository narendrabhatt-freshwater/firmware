/**
 ******************************************************************************
 * @file    note_bank.h
 * @brief   SAMPLE voice bank: one playhead over attack RAM + body slots.
 ******************************************************************************
 */

#ifndef __NOTE_BANK_H__
#define __NOTE_BANK_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stddef.h>

#include "attack_bank.h"
#include "freshwater/vm.h"

/** Voices available in SAMPLE mode (n0..n7). */
#define NOTE_BANK_VOICES SAMPLE_VOICES

  /** Kept for console compatibility; SAMPLE ignores oscillator shape. */
  typedef enum
  {
    NOTE_SHAPE_SINE = 0,
    NOTE_SHAPE_PULSE = 1,
    NOTE_SHAPE_TRI = 2
  } NoteBank_Shape_t;

  void NoteBank_Init(void);
  void NoteBank_PanicAll(void);

  /** Crash-in tail length. Values are clamped to 0..50 ms (0 = hard cut). */
  void NoteBank_SetCrashReleaseMs(uint8_t release_ms);
  uint8_t NoteBank_GetCrashReleaseMs(void);

  /**
   * @brief Note on/off. freq_hz <= 0 releases/stops.
   * freq_hz > 0 is always a note-on (restarts attack + body). Applied on
   * the next I2S sample. Voice uses the assigned wave_id (`aw`).
   */
  /** Returns -2 when this voice has no active VM program. */
  int NoteBank_SetFreq(uint8_t note, double freq_hz, double scale);

  /** Streamed note-on variant. The session is bound to nX before its ACK so
   * only the matching USB SOF can claim the replacement ring. */
  int NoteBank_SetFreqSession(uint8_t note, double freq_hz, double scale,
                              uint8_t session);

  double NoteBank_GetFreq(uint8_t note);
  double NoteBank_GetScale(uint8_t note);

  /** Assign AXI head 0..255 to voice 0..7. Applied on the next note-on. */
  int NoteBank_SetWaveId(uint8_t note, uint16_t wave_id);
  uint16_t NoteBank_GetWaveId(uint8_t note);

  int NoteBank_SetShape(NoteBank_Shape_t shape, double param);
  NoteBank_Shape_t NoteBank_GetShape(void);
  double NoteBank_GetShapeParam(void);

  /** Body FIFO miss counter. A published-body underrun now halts instead. */
  uint32_t NoteBank_HoldCount(void);
  void NoteBank_HoldCountClear(void);

  /** True while attack/sustain/release still sounds (incl. env release). */
  uint8_t NoteBank_IsActive(uint8_t note);
  uint8_t NoteBank_AnyActive(void);
  int32_t NoteBank_NextSample(void);

  /** VM hooks bracketing one 48-sample DMA refill. */
  void NoteBank_VmBoundaryBegin(void);
  void NoteBank_VmBoundaryEnd(void);
  void NoteBank_VmStop(uint8_t voice);
  void NoteBank_VmStopAll(void);
  uint8_t NoteBank_VmIsActive(uint8_t voice);
  uint8_t NoteBank_VmActiveMask(void);
  /** Chunked FWSC upload. Invalid uploads preserve the active program. */
  int NoteBank_VmUploadBegin(uint8_t voice);
  int NoteBank_VmUploadFeed(uint8_t voice, const void *data, size_t size);
  int NoteBank_VmUploadCommit(uint8_t voice);
  void NoteBank_VmUploadAbort(uint8_t voice);
  uint8_t NoteBank_VmUploadIsActive(uint8_t voice);
  uint8_t NoteBank_VmUploadIsBusy(void);
  const FwVmMemoryMetrics *NoteBank_VmMemoryMetrics(void);
  FwVmFault NoteBank_VmFault(uint8_t voice);
  uint32_t NoteBank_VmMaxCycles(uint8_t voice);
  uint32_t NoteBank_VmFaultCount(uint8_t voice);

  /** Active mask and hungriest voice (0xFF if none). */
  void NoteBank_VoiceQuery(uint8_t *mask_out, uint8_t *best_out);

#ifdef __cplusplus
}
#endif

#endif /* __NOTE_BANK_H__ */
