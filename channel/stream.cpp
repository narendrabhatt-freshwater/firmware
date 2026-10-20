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

#include "stream.h"
#include "usb.h"
#include <stddef.h>
#include <string.h>

/* ---- queue streamed body samples ---------------------------------------- */

#define BLOCK 16u
#define BLOCKS 255u
#define NONE 255u
#define QUEUES 3u
#define NODES (BLOCKS + QUEUES)
#define NO_NODE 511u
static_assert(__atomic_always_lock_free(4, 0), "32-bit atomic publication required");

typedef struct {
    uint32_t generation, published, position;
    uint16_t consumed, wave, head, tail, reclaim, completed, reclaimed;
    uint8_t session, skip, allocated, dummy;
} Queue;

typedef struct {
    Queue queue[QUEUES];
    uint32_t state, retired;
    uint16_t free_node, free_count;
    uint8_t free_head, initialized, consuming;
    uint16_t info[NODES];
    uint8_t block[NODES];
    int8_t data[STREAM_RING_SAMPLES];
} Ring;

static Ring rings[SAMPLE_VOICES] __attribute__((aligned(32)));
static volatile uint32_t s_drop_pkts, s_rx_pkts, s_sof_pkts, s_zero_pkts;
static volatile uint32_t s_stale_pkts, s_future_pkts, s_full_pkts, s_superseded_pkts;
static volatile uint16_t s_last_body_sequence;
static volatile uint32_t s_audio_frames;
static uint32_t s_last_body_frame;
static uint8_t s_have_body;
static volatile uint32_t s_min_fill = 0xFFFFFFFFu;

/* ---- publish and read ring state ---------------------------------------- */

static uint32_t load_stream_state(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void publish_stream_state(uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static unsigned get_current_queue_index(uint32_t s)
{
    return s & 255u;
}

static unsigned get_pending_queue_index(uint32_t s)
{
    return (s >> 8) & 255u;
}

static uint32_t make_next_stream_state(uint32_t s, unsigned c, unsigned p)
{
    return ((s + 65536u) & 0xffff0000u) | (p << 8) | c;
}

static int compare_exchange_stream_state(Ring *r, uint32_t *s, unsigned c, unsigned p)
{
    return __atomic_compare_exchange_n(&r->state, s, make_next_stream_state(*s, c, p), 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static void retire_stream_queue(Ring *r, unsigned q)
{
    if (q < QUEUES) __atomic_fetch_or(&r->retired, 1u << q, __ATOMIC_RELEASE);
}

/* ---- manage ring storage nodes ------------------------------------------ */

static unsigned get_next_stream_node(Ring *r, unsigned n)
{
    return r->info[n] & 511u;
}

static unsigned get_stream_node_sample_count(Ring *r, unsigned n)
{
    return r->info[n] >> 9;
}

static void set_next_stream_node(Ring *r, unsigned n, unsigned next)
{
    r->info[n] = (uint16_t)((r->info[n] & ~511u) | next);
}

static unsigned allocate_stream_node(Ring *r)
{
    unsigned n = r->free_node;
    if (n == NO_NODE) __builtin_trap();
    r->free_node = (uint16_t)get_next_stream_node(r, n);
    r->info[n] = NO_NODE;
    r->block[n] = NONE;
    return n;
}

static void free_stream_node(Ring *r, unsigned n)
{
    set_next_stream_node(r, n, r->free_node);
    r->free_node = (uint16_t)n;
}

/* ---- initialize and reclaim ring storage -------------------------------- */

static void init_stream_storage(Ring *r)
{
    if (r->initialized) return;
    for (unsigned i = 0; i < BLOCKS; i++)
        r->data[i * BLOCK] = (int8_t)(i + 1u);
    for (unsigned i = 0; i < NODES; i++)
        r->info[i] = (uint16_t)(i + 1u);
    r->info[NODES - 1] = NO_NODE;
    r->free_node = 0;
    r->free_head = 0;
    r->free_count = BLOCKS;
    r->state = 0xffffu;
    r->initialized = 1;
}

static void free_stream_data_block(Ring *r, Queue *q, unsigned n)
{
    unsigned b = r->block[n];
    if (b == NONE) return;
    r->data[b * BLOCK] = (int8_t)r->free_head;
    r->free_head = (uint8_t)b;
    ++r->free_count;
    r->block[n] = NONE;
    if (q) ++q->reclaimed;
}

static void reclaim_stream_storage(Ring *r)
{
    uint32_t retired = __atomic_exchange_n(&r->retired, 0, __ATOMIC_ACQ_REL);
    for (unsigned i = 0; i < QUEUES; i++) {
        Queue *q = &r->queue[i];
        if (!q->allocated) continue;
        uint32_t pos = load_stream_state(&q->position);
        unsigned head = pos & 511u;
        unsigned n = q->reclaim;
        while (n != NO_NODE && (n != head || (retired & (1u << i)))) {
            unsigned next = get_next_stream_node(r, n);
            free_stream_data_block(r, q, n);
            free_stream_node(r, n);
            n = next;
        }
        q->reclaim = (uint16_t)n;
        if (retired & (1u << i))
            q->allocated = 0;
        else if (pos & 512u)
            free_stream_data_block(r, q, head);
    }
}

/* ---- reset voice rings -------------------------------------------------- */

void reset_voice_stream(uint8_t v)
{
    if (v >= SAMPLE_VOICES) return;
    Ring *r = &rings[v];
    init_stream_storage(r);
    uint32_t s = load_stream_state(&r->state), old;
    do {
        old = s;
    } while (!compare_exchange_stream_state(r, &s, NONE, NONE));
    retire_stream_queue(r, get_current_queue_index(old));
    retire_stream_queue(r, get_pending_queue_index(old));
    r->consuming = 0;
}

void reset_all_voice_streams(void)
{
    for (unsigned v = 0; v < SAMPLE_VOICES; v++)
        reset_voice_stream(v);
}

void init_voice_streams(void)
{
    s_last_body_sequence = 0;
    s_audio_frames = s_last_body_frame = 0;
    s_have_body = 0;
    reset_all_voice_streams();
}

/* ---- manage current and pending sessions -------------------------------- */

void mark_voice_stream_consuming(uint8_t v)
{
    if (v < SAMPLE_VOICES) rings[v].consuming = 1;
}

void prepare_pending_voice_stream(uint8_t v, uint16_t wave, uint8_t session)
{
    if (v >= SAMPLE_VOICES) return;
    Ring *r = &rings[v];
    init_stream_storage(r);
    reclaim_stream_storage(r);
    unsigned n = 0;
    while (n < QUEUES && r->queue[n].allocated)
        ++n;
    if (n == QUEUES) __builtin_trap();
    Queue *q = &r->queue[n];
    q->allocated = 1;
    ++q->generation;
    q->consumed = q->completed = q->reclaimed = 0;
    q->skip = 0;
    q->head = q->tail = q->reclaim = (uint16_t)allocate_stream_node(r);
    q->dummy = 1;
    q->wave = wave;
    q->session = session;
    publish_stream_state(&q->published, 0);
    publish_stream_state(&q->position, 512u | q->head);
    uint32_t s = load_stream_state(&r->state), old;
    do {
        old = s;
    } while (!compare_exchange_stream_state(r, &s, get_current_queue_index(s), n));
    retire_stream_queue(r, get_pending_queue_index(old));
}

int activate_pending_voice_stream(uint8_t v)
{
    if (v >= SAMPLE_VOICES) return -1;
    Ring *r = &rings[v];
    uint32_t s = load_stream_state(&r->state), old;
    do {
        old = s;
        if (get_pending_queue_index(s) >= QUEUES) return -1;
    } while (!compare_exchange_stream_state(r, &s, get_pending_queue_index(s), NONE));
    retire_stream_queue(r, get_current_queue_index(old));
    r->consuming = 1;
    return 0;
}

void discard_pending_voice_stream(uint8_t v)
{
    if (v >= SAMPLE_VOICES) return;
    Ring *r = &rings[v];
    uint32_t s = load_stream_state(&r->state), old;
    do {
        old = s;
    } while (!compare_exchange_stream_state(r, &s, get_current_queue_index(s), NONE));
    retire_stream_queue(r, get_pending_queue_index(old));
}

void end_current_voice_stream(uint8_t v)
{
    if (v >= SAMPLE_VOICES) return;
    Ring *r = &rings[v];
    uint32_t s = load_stream_state(&r->state), old;
    do {
        old = s;
    } while (!compare_exchange_stream_state(r, &s, NONE, get_pending_queue_index(s)));
    retire_stream_queue(r, get_current_queue_index(old));
    r->consuming = 0;
}

void release_voice_stream(uint8_t v)
{
    reset_voice_stream(v);
}

/* ---- query ring sessions and capacity ----------------------------------- */

uint8_t get_current_stream_session(uint8_t v)
{
    if (v >= SAMPLE_VOICES) return NONE;
    Ring *r = &rings[v];
    unsigned i = get_current_queue_index(load_stream_state(&r->state));
    return i < QUEUES ? r->queue[i].session : NONE;
}

static uint32_t get_queue_buffered_samples(Queue *q)
{
    return (uint16_t)((load_stream_state(&q->published) >> 16) - q->consumed);
}

uint32_t get_current_stream_buffered_samples(uint8_t v)
{
    if (v >= SAMPLE_VOICES) return 0;
    Ring *r = &rings[v];
    unsigned i = get_current_queue_index(load_stream_state(&r->state));
    return i < QUEUES ? get_queue_buffered_samples(&r->queue[i]) : 0;
}

uint32_t get_pending_stream_buffered_samples(uint8_t v)
{
    if (v >= SAMPLE_VOICES) return 0;
    Ring *r = &rings[v];
    unsigned i = get_pending_queue_index(load_stream_state(&r->state));
    return i < QUEUES ? get_queue_buffered_samples(&r->queue[i]) : 0;
}

uint32_t get_stream_buffered_samples(uint8_t v)
{
    return get_current_stream_buffered_samples(v);
}

uint32_t get_stream_free_samples(uint8_t v)
{
    if (v >= SAMPLE_VOICES) return 0;
    Ring *r = &rings[v];
    uint32_t count = r->free_count, retired = load_stream_state(&r->retired);
    for (unsigned i = 0; i < QUEUES; i++) {
        Queue *q = &r->queue[i];
        if (!q->allocated) continue;
        uint16_t available =
            (uint16_t)((retired & (1u << i)) ? load_stream_state(&q->published)
                                           : (load_stream_state(&q->position) >> 16));
        count += (uint16_t)(available - q->reclaimed);
    }
    return count * BLOCK;
}

uint32_t get_current_stream_free_samples(uint8_t v)
{
    return get_stream_free_samples(v);
}

uint8_t has_pending_voice_stream(uint8_t v)
{
    return v < SAMPLE_VOICES &&
           get_pending_queue_index(load_stream_state(&rings[v].state)) < QUEUES;
}

uint8_t get_target_stream_session(uint8_t v)
{
    if (v >= SAMPLE_VOICES) return NONE;
    Ring *r = &rings[v];
    uint32_t s = load_stream_state(&r->state);
    unsigned i = get_pending_queue_index(s);
    if (i >= QUEUES) i = get_current_queue_index(s);
    return i < QUEUES ? r->queue[i].session : NONE;
}

uint32_t get_target_stream_buffered_samples(uint8_t v)
{
    return has_pending_voice_stream(v) ? get_pending_stream_buffered_samples(v)
                                      : get_current_stream_buffered_samples(v);
}

/* ---- reserve an incoming body block ------------------------------------- */

int begin_stream_write(uint8_t v, uint8_t session, uint8_t sof, uint16_t wave, uint32_t n,
                       StreamRing_Write_t *w)
{
    if (!w) return STREAM_RING_WRITE_ERROR;
    memset(w, 0, sizeof *w);
    if (v >= SAMPLE_VOICES || !n || n > USB_STREAM_NSAMP_MAX) return STREAM_RING_WRITE_ERROR;
    Ring *r = &rings[v];
    reclaim_stream_storage(r);
    uint32_t s = load_stream_state(&r->state);
    unsigned qi = get_pending_queue_index(s);
    if (qi >= QUEUES || r->queue[qi].wave != wave ||
        (r->queue[qi].session != session && !(sof && r->queue[qi].session == NONE)))
        qi = get_current_queue_index(s);
    if (qi >= QUEUES || r->queue[qi].wave != wave || session == NONE ||
        (r->queue[qi].session != session && !(sof && r->queue[qi].session == NONE))) {
        ++s_stale_pkts;
        return STREAM_RING_WRITE_STALE;
    }
    Queue *q = &r->queue[qi];
    q->session = session;
    unsigned blocks = (n + BLOCK - 1) / BLOCK;
    if (blocks > r->free_count) {
        ++s_drop_pkts;
        ++s_full_pkts;
        return STREAM_RING_WRITE_ERROR;
    }
    w->voice = v;
    w->session = session;
    w->bank = (uint8_t)qi;
    w->generation = q->generation;
    w->pending = qi == get_pending_queue_index(s);
    w->sof = sof;
    w->nsamp = n;
    w->active = 1;
    unsigned left = n;
    w->head = w->tail = w->cursor = NO_NODE;
    for (unsigned i = 0; i < blocks; i++) {
        unsigned node = allocate_stream_node(r), b = r->free_head;
        r->free_head = (uint8_t)r->data[b * BLOCK];
        --r->free_count;
        r->block[node] = (uint8_t)b;
        unsigned length = left > BLOCK ? BLOCK : left;
        left -= length;
        r->info[node] = (uint16_t)((length << 9) | NO_NODE);
        if (w->head == NO_NODE)
            w->head = w->cursor = (uint16_t)node;
        else
            set_next_stream_node(r, w->tail, node);
        w->tail = (uint16_t)node;
    }
    return STREAM_RING_WRITE_OK;
}

uint8_t is_stream_write_current(const StreamRing_Write_t *w)
{
    if (!w || !w->active || w->voice >= SAMPLE_VOICES || w->bank >= QUEUES) return 0;
    Ring *r = &rings[w->voice];
    uint32_t s = load_stream_state(&r->state);
    Queue *q = &r->queue[w->bank];
    return (get_current_queue_index(s) == w->bank || get_pending_queue_index(s) == w->bank) &&
           q->generation == w->generation &&
           q->session == w->session;
}

int8_t *get_stream_write_span(StreamRing_Write_t *w, uint32_t *n)
{
    if (n) *n = 0;
    if (!w || !w->active || w->cursor == NO_NODE || w->written >= w->nsamp) return NULL;
    Ring *r = &rings[w->voice];
    if (n) *n = get_stream_node_sample_count(r, w->cursor) - w->offset;
    return &r->data[r->block[w->cursor] * BLOCK + w->offset];
}

int advance_stream_write(StreamRing_Write_t *w, uint32_t n)
{
    if (!w || !w->active || w->cursor == NO_NODE || !n) return -1;
    Ring *r = &rings[w->voice];
    if (n > (uint32_t)(get_stream_node_sample_count(r, w->cursor) - w->offset)) return -1;
    w->written += n;
    w->offset += (uint8_t)n;
    if (w->offset == get_stream_node_sample_count(r, w->cursor)) {
        w->cursor = (uint16_t)get_next_stream_node(r, w->cursor);
        w->offset = 0;
    }
    return 0;
}

void abort_stream_write(StreamRing_Write_t *w)
{
    if (!w || !w->active) return;
    Ring *r = &rings[w->voice];
    unsigned n = w->head;
    while (n != NO_NODE) {
        unsigned next = get_next_stream_node(r, n);
        free_stream_data_block(r, NULL, n);
        free_stream_node(r, n);
        n = next;
    }
    w->active = 0;
}

/* ---- commit received body samples --------------------------------------- */

uint32_t commit_stream_write(StreamRing_Write_t *w)
{
    if (!w || !w->active || w->written != w->nsamp) return 0;
    Ring *r = &rings[w->voice];
    uint8_t zero = 1;
    for (unsigned n = w->head; n != NO_NODE; n = get_next_stream_node(r, n))
        for (unsigned i = 0; i < get_stream_node_sample_count(r, n); i++)
            if (r->data[r->block[n] * BLOCK + i]) zero = 0;
    if (!is_stream_write_current(w)) {
        abort_stream_write(w);
        return 0;
    }
    Queue *q = &r->queue[w->bank];
    uint32_t pub = load_stream_state(&q->published);
    uint16_t wr = (uint16_t)pub, total = (uint16_t)(pub >> 16);
    set_next_stream_node(r, q->tail, w->head);
    q->tail = w->tail;
    wr = (uint16_t)(wr + (w->nsamp + BLOCK - 1) / BLOCK);
    w->active = 0;
    publish_stream_state(&q->published, ((uint32_t)(uint16_t)(total + w->nsamp) << 16) | wr);
    ++s_rx_pkts;
    if (zero) ++s_zero_pkts;
    if (w->sof) ++s_sof_pkts;
    return w->nsamp;
}

/* ---- write a complete body block ---------------------------------------- */

uint32_t write_voice_stream_samples(uint8_t v, uint8_t session, uint8_t sof, uint16_t wave,
                                    const int8_t *s, uint32_t n)
{
    if (!s) return 0;
    StreamRing_Write_t w;
    if (begin_stream_write(v, session, sof, wave, n, &w) != 0) return 0;
    while (w.written < n) {
        uint32_t span;
        int8_t *d = get_stream_write_span(&w, &span);
        if (!d || !span) {
            abort_stream_write(&w);
            return 0;
        }
        memcpy(d, s + w.written, span);
        if (advance_stream_write(&w, span)) {
            abort_stream_write(&w);
            return 0;
        }
    }
    return commit_stream_write(&w);
}

int write_stream_body_samples(uint8_t v, uint8_t session, const int8_t *s, uint16_t n)
{
    if (v >= SAMPLE_VOICES || !s || !n || n > USB_STREAM_PAYLOAD_MAX) return -1;
    Ring *r = &rings[v];
    uint32_t state = load_stream_state(&r->state);
    unsigned qi = get_pending_queue_index(state);
    if (qi >= QUEUES || r->queue[qi].session != session) qi = get_current_queue_index(state);
    if (qi >= QUEUES || r->queue[qi].session != session) {
        ++s_stale_pkts;
        ++s_last_body_sequence;
        return 0;
    }
    Queue *q = &r->queue[qi];
    if (((n + BLOCK - 1) / BLOCK) * BLOCK > get_stream_free_samples(v)) return -1;
    unsigned accepted =
        write_voice_stream_samples(v, session,
                                   qi == get_pending_queue_index(state) &&
                                       get_queue_buffered_samples(q) == 0,
                                   q->wave, s, n);
    s_last_body_frame = s_audio_frames;
    s_have_body = 1;
    ++s_last_body_sequence;
    return accepted == n ? 1 : 0;
}

/* ---- consume samples from the current ring ------------------------------ */

int peek_stream_samples(uint8_t v, uint32_t offset, int8_t *out, uint32_t count)
{
    if (v >= SAMPLE_VOICES || !out) return -1;
    Ring *r = &rings[v];
    unsigned qi = get_current_queue_index(load_stream_state(&r->state));
    if (qi >= QUEUES) return -1;
    Queue *q = &r->queue[qi];
    uint32_t available = get_queue_buffered_samples(q);
    if (offset > available || count > available - offset) return -1;
    offset += q->skip;
    unsigned n = q->dummy ? get_next_stream_node(r, q->head) : q->head;
    while (count) {
        uint32_t length = get_stream_node_sample_count(r, n);
        if (offset >= length) {
            offset -= length;
            n = get_next_stream_node(r, n);
            continue;
        }
        uint32_t take = length - offset;
        if (take > count) take = count;
        for (uint32_t i = 0; i < take; i++)
            *out++ = r->data[r->block[n] * BLOCK + offset + i];
        count -= take;
        offset = 0;
        n = get_next_stream_node(r, n);
    }
    return 0;
}

uint32_t read_stream_samples(uint8_t v, int8_t *out, uint32_t count)
{
    if (v >= SAMPLE_VOICES || !count) return 0;
    Ring *r = &rings[v];
    unsigned qi = get_current_queue_index(load_stream_state(&r->state));
    if (qi >= QUEUES) return 0;
    Queue *q = &r->queue[qi];
    uint32_t available = get_queue_buffered_samples(q);
    if (count > available) count = available;
    uint32_t consumed = count;
    unsigned changed = 0;
    q->consumed += (uint16_t)count;
    while (count) {
        if (q->dummy) {
            q->head = (uint16_t)get_next_stream_node(r, q->head);
            q->dummy = 0;
            changed = 1;
        }
        unsigned n = q->head;
        uint32_t length = get_stream_node_sample_count(r, n) - q->skip;
        uint32_t take = count < length ? count : length;
        if (out)
            for (uint32_t i = 0; i < take; i++)
                *out++ = r->data[r->block[n] * BLOCK + q->skip + i];
        count -= take;
        if (take < length) {
            q->skip += (uint8_t)take;
            break;
        }
        ++q->completed;
        q->skip = 0;
        q->dummy = 1;
        changed = 1;
    }
    if (changed)
        publish_stream_state(&q->position,
                             ((uint32_t)q->completed << 16) | (q->dummy ? 512u : 0u) | q->head);
    if (r->consuming && available - consumed < s_min_fill) s_min_fill = available - consumed;
    return consumed;
}

int peek_stream_sample(uint8_t v, uint32_t offset, int8_t *out)
{
    return peek_stream_samples(v, offset, out, 1);
}

void skip_stream_samples(uint8_t v, uint32_t n)
{
    (void)read_stream_samples(v, NULL, n);
}

/* ---- query and clear ring statistics ------------------------------------ */

uint32_t get_max_stream_buffered_samples(void)
{
    uint32_t n = 0;
    for (unsigned v = 0; v < SAMPLE_VOICES; v++) {
        uint32_t f = get_current_stream_buffered_samples(v);
        if (f > n) n = f;
    }
    return n;
}

void record_stream_buffered_samples(uint8_t v)
{
    if (v < SAMPLE_VOICES && rings[v].consuming) {
        uint32_t f = get_current_stream_buffered_samples(v);
        if (f < s_min_fill) s_min_fill = f;
    }
}

uint16_t get_last_stream_body_sequence(void)
{
    return s_last_body_sequence;
}

uint32_t get_min_stream_buffered_samples(void)
{
    return s_min_fill;
}

uint32_t get_stream_dropped_block_count(void)
{
    return s_drop_pkts;
}

uint32_t get_stream_received_block_count(void)
{
    return s_rx_pkts;
}

uint32_t get_stream_start_block_count(void)
{
    return s_sof_pkts;
}

uint32_t get_stream_silent_block_count(void)
{
    return s_zero_pkts;
}

uint32_t get_stream_stale_block_count(void)
{
    return s_stale_pkts;
}

uint32_t get_stream_future_block_count(void)
{
    return s_future_pkts;
}

uint32_t get_stream_full_buffer_count(void)
{
    return s_full_pkts;
}

uint32_t get_stream_superseded_block_count(void)
{
    return s_superseded_pkts;
}

void clear_stream_statistics(void)
{
    s_drop_pkts = 0u;
    s_rx_pkts = 0u;
    s_sof_pkts = 0u;
    s_zero_pkts = 0u;
    s_stale_pkts = 0u;
    s_future_pkts = 0u;
    s_full_pkts = 0u;
    s_superseded_pkts = 0u;
    s_min_fill = 0xFFFFFFFFu;
}

void reset_stream_statistics(void)
{
    clear_stream_statistics();
}

void record_stream_audio_sample(void)
{
    ++s_audio_frames;
}

uint8_t get_stream_body_age_ms(void)
{
    uint32_t frames = s_audio_frames - s_last_body_frame;
    if (s_have_body == 0u || frames >= 254u * 48u) return 255u;
    return (uint8_t)((frames + 47u) / 48u);
}
