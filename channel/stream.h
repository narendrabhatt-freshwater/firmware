/*
                                   __
                               ___  \  \
                          ___  \  \  \  \    _______
                     ___  \  \  \  \  \__\__/_____  \
                     \  \  \  \  \__\_______      \__\____
                      \  \  \  \     ___ \  \________  \__/
                       \  \  \__\___/_  \ \___/   \  \___
                        \  \    ____  \  \________ \ ___/
                         \__\__/ \  \  \____/  \  \
                                  \  \     ___  \  \___
                                   \__\___/ \  \ \____/
                                           \  \
                                              \  \___
                 __                 _          \ ___/  _
                / _|_ __  ___  ___ | |____      ____ _| |_  ___  _ __
               | |_| '_ |/ _ \/ __|| '_ \ \ /\ / / _` | __|/ _ \| '__|
               |  _| |  |  __/\__ \| | | \ V  V / (_| | |_|  __/| |
               |_| |_|   \___||___/|_| |_|\_/\_/ \__,_|\__|\___||_|

            (C) 2 0 2 6   F r e s h w a t e r   I n s t r u m e n t s
*/

#ifndef CHANNEL_APP_STREAM_H
#define CHANNEL_APP_STREAM_H

#include <stdint.h>
#include "samples.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- queue streamed samples --------------------------------------------- */

/* Per-voice BODY FIFOs stored contiguously in DTCM, filled from USB.
 * Each CDC BODY block carries a voice/session and up to 1024 signed-int8
 * samples. The firmware counts processed blocks for cumulative RS485 ACKs.
 * Note-on arms the replacement origin; its first matching BODY starts it. */

/** 85 ms of signed-int8 BODY data per voice at 48 kHz. */
#define STREAM_RING_SAMPLES 4080u

/** One in-progress producer reservation. Samples remain invisible to the
 *  audio consumer until commit_stream_write advances the ring writer. */
/* ---- producer reservation ----------------------------------------------- */

typedef struct {
    uint32_t start_wr;
    uint32_t nsamp;
    uint32_t written;
    uint32_t generation;
    uint8_t voice;
    uint8_t session;
    uint8_t sof;
    uint8_t pending;
    uint8_t bank, offset;
    uint16_t head, tail, cursor;
    uint8_t active;
} StreamRing_Write_t;

#define STREAM_RING_WRITE_OK 0
#define STREAM_RING_WRITE_STALE 1
#define STREAM_RING_WRITE_FUTURE 3
#define STREAM_RING_WRITE_ERROR (-1)

/* ---- ring lifecycle ----------------------------------------------------- */

void init_voice_streams(void);
void reset_voice_stream(uint8_t voice);
void reset_all_voice_streams(void);

/**
 * @brief Arm consumption of the current ring session.
 */
void mark_voice_stream_consuming(uint8_t voice);

/** Create an empty pending generation. Superseding only drops old pending. */
void prepare_pending_voice_stream(uint8_t voice, uint16_t wave_id, uint8_t session);

/** Atomically discard current remainder and promote the pending origin. */
int activate_pending_voice_stream(uint8_t voice);

/** Reject an incoming generation without disturbing current playback. */
void discard_pending_voice_stream(uint8_t voice);

/** End only the current generation; a pending generation remains armed. */
void end_current_voice_stream(uint8_t voice);

/** Stop consume and empty the FIFO (note-off). */
void release_voice_stream(uint8_t voice);

/**
 * @brief Push one BODY burst into a voice ring.
 * @return nsamp accepted, or 0 if the whole burst would overflow / bad args.
 */
/* ---- receive body samples ----------------------------------------------- */

uint32_t write_voice_stream_samples(uint8_t voice, uint8_t session, uint8_t sof, uint16_t wave_id,
                                    const int8_t *samples, uint32_t nsamp);

/** One complete CDC BODY block: 1 accepted, 0 retired session, -1 capacity/error. */
int write_stream_body_samples(uint8_t voice, uint8_t session, const int8_t *samples,
                              uint16_t count);
uint16_t get_last_stream_body_sequence(void);
void record_stream_audio_sample(void);
uint8_t get_stream_body_age_ms(void);

/** Reserve a complete BODY burst without publishing it to the consumer.
 * @retval STREAM_RING_WRITE_OK reservation active
 * @retval STREAM_RING_WRITE_STALE prior note/session is no longer active
 * @retval STREAM_RING_WRITE_FUTURE SOF arrived before its tagged nX
 * @retval STREAM_RING_WRITE_ERROR invalid request or insufficient credit */
/* ---- reserve and commit body blocks ------------------------------------- */

int begin_stream_write(uint8_t voice, uint8_t session, uint8_t sof, uint16_t wave_id,
                       uint32_t nsamp, StreamRing_Write_t *write);

/** Non-zero only while the note/session that owns this reservation exists. */
uint8_t is_stream_write_current(const StreamRing_Write_t *write);

/** Contiguous writable portion of a reservation, bounded by bank/wrap. */
int8_t *get_stream_write_span(StreamRing_Write_t *write, uint32_t *nsamp_out);

/** Record samples placed in the current writable span. */
int advance_stream_write(StreamRing_Write_t *write, uint32_t nsamp);

/** Atomically publish a completely written reservation. */
uint32_t commit_stream_write(StreamRing_Write_t *write);

/** Abandon an incomplete reservation; no samples become visible. */
void abort_stream_write(StreamRing_Write_t *write);

/**
 * @brief Body sample at offset from rd (0 = next unread).
 * @retval 0 ok, *out set
 * @retval -1 offset past wr (underrun / not yet written)
 */
/* ---- read body samples -------------------------------------------------- */

int peek_stream_sample(uint8_t voice, uint32_t offset, int8_t *out);
int peek_stream_samples(uint8_t voice, uint32_t offset, int8_t *out, uint32_t count);
uint32_t read_stream_samples(uint8_t voice, int8_t *out, uint32_t count);

/** Drop up to n unread samples from rd (playhead consumed them). */
void skip_stream_samples(uint8_t voice, uint32_t n);

/* ---- ring state and diagnostics ----------------------------------------- */

uint32_t get_current_stream_buffered_samples(uint8_t voice);
uint32_t get_pending_stream_buffered_samples(uint8_t voice);
uint32_t get_stream_buffered_samples(uint8_t voice);

uint32_t get_stream_free_samples(uint8_t voice);

uint8_t get_current_stream_session(uint8_t voice);
uint32_t get_current_stream_free_samples(uint8_t voice);
uint8_t has_pending_voice_stream(uint8_t voice);
uint8_t get_target_stream_session(uint8_t voice);
uint32_t get_target_stream_buffered_samples(uint8_t voice);

/** Highest fill among all voices (feedback / console). */
uint32_t get_max_stream_buffered_samples(void);

/** Lowest fill seen on a consuming voice since the last stats clear. */
uint32_t get_min_stream_buffered_samples(void);

/** Record buffered samples during rendering. */
void record_stream_buffered_samples(uint8_t voice);

/** Whole BODY bursts dropped because the FIFO was full. */
uint32_t get_stream_dropped_block_count(void);

/** BODY bursts accepted into a ring. */
uint32_t get_stream_received_block_count(void);

/** Accepted armed session starts (SOF + new session). */
uint32_t get_stream_start_block_count(void);

/** Bursts whose samples were all zero. */
uint32_t get_stream_silent_block_count(void);
uint32_t get_stream_stale_block_count(void);
uint32_t get_stream_future_block_count(void);
uint32_t get_stream_full_buffer_count(void);
uint32_t get_stream_superseded_block_count(void);

void clear_stream_statistics(void);

void reset_stream_statistics(void);

#ifdef __cplusplus
}
#endif

#endif
