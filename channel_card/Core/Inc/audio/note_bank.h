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

#if defined(CHANNEL_TEST_WAVETABLE)
  /** Test-build-only oscillator shapes; absent from production firmware. */
  typedef enum
  {
    NOTE_SHAPE_SINE = 0,
    NOTE_SHAPE_PULSE = 1,
    NOTE_SHAPE_TRI = 2,
    NOTE_SHAPE_SAW = 3
  } NoteBank_Shape_t;
#endif

  void NoteBank_Init(void);
  void NoteBank_PanicAll(void);

  /** Raw MIDI-key note-on. The per-voice script map and tuning select pitch. */
  int NoteBank_NoteOn(uint8_t note, uint8_t key);

  /** Streamed note-on variant. The session is bound to nX before its ACK so
   * only the matching USB SOF can claim the replacement ring. */
  int NoteBank_NoteOnSession(uint8_t note, uint8_t key, uint8_t session);
  int NoteBank_NoteOff(uint8_t note);

  uint8_t NoteBank_GetKey(uint8_t note);
  uint8_t NoteBank_GetMappedKey(uint8_t note);
  double NoteBank_GetFreq(uint8_t note);

  /** Assign AXI head 0..255 to voice 0..7. Applied on the next note-on. */
  int NoteBank_SetWaveId(uint8_t note, uint16_t wave_id);
  uint16_t NoteBank_GetWaveId(uint8_t note);

#if defined(CHANNEL_TEST_WAVETABLE)
  int NoteBank_SetShape(NoteBank_Shape_t shape, double param);
#endif

  /** Body FIFO miss counter. Production firmware halts on the first miss. */
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
