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

static uint32_t load(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void publish(uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static unsigned current(uint32_t s)
{
    return s & 255u;
}

static unsigned pending(uint32_t s)
{
    return (s >> 8) & 255u;
}

static uint32_t state_next(uint32_t s, unsigned c, unsigned p)
{
    return ((s + 65536u) & 0xffff0000u) | (p << 8) | c;
}

static int change(Ring *r, uint32_t *s, unsigned c, unsigned p)
{
    return __atomic_compare_exchange_n(&r->state, s, state_next(*s, c, p), 0, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}

static void retire(Ring *r, unsigned q)
{
    if (q < QUEUES) __atomic_fetch_or(&r->retired, 1u << q, __ATOMIC_RELEASE);
}

/* ---- manage ring storage nodes ------------------------------------------ */

static unsigned next_node(Ring *r, unsigned n)
{
    return r->info[n] & 511u;
}

static unsigned node_length(Ring *r, unsigned n)
{
    return r->info[n] >> 9;
}

static void set_next(Ring *r, unsigned n, unsigned next)
{
    r->info[n] = (uint16_t)((r->info[n] & ~511u) | next);
}

static unsigned allocate_node(Ring *r)
{
    unsigned n = r->free_node;
    if (n == NO_NODE) __builtin_trap();
    r->free_node = (uint16_t)next_node(r, n);
    r->info[n] = NO_NODE;
    r->block[n] = NONE;
    return n;
}

static void free_node(Ring *r, unsigned n)
{
    set_next(r, n, r->free_node);
    r->free_node = (uint16_t)n;
}

/* ---- initialize and reclaim ring storage -------------------------------- */

static void initialize(Ring *r)
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

static void free_data(Ring *r, Queue *q, unsigned n)
{
    unsigned b = r->block[n];
    if (b == NONE) return;
    r->data[b * BLOCK] = (int8_t)r->free_head;
    r->free_head = (uint8_t)b;
    ++r->free_count;
    r->block[n] = NONE;
    if (q) ++q->reclaimed;
}

static void reclaim(Ring *r)
{
    uint32_t retired = __atomic_exchange_n(&r->retired, 0, __ATOMIC_ACQ_REL);
    for (unsigned i = 0; i < QUEUES; i++) {
        Queue *q = &r->queue[i];
        if (!q->allocated) continue;
        uint32_t pos = load(&q->position);
        unsigned head = pos & 511u;
        unsigned n = q->reclaim;
        while (n != NO_NODE && (n != head || (retired & (1u << i)))) {
            unsigned next = next_node(r, n);
            free_data(r, q, n);
            free_node(r, n);
            n = next;
        }
        q->reclaim = (uint16_t)n;
        if (retired & (1u << i))
            q->allocated = 0;
        else if (pos & 512u)
            free_data(r, q, head);
    }
}

/* ---- reset voice rings -------------------------------------------------- */

void StreamRing_Reset(uint8_t v)
{
    if (v >= SAMPLE_VOICES) return;
    Ring *r = &rings[v];
    initialize(r);
    uint32_t s = load(&r->state), old;
    do {
        old = s;
    } while (!change(r, &s, NONE, NONE));
    retire(r, current(old));
    retire(r, pending(old));
    r->consuming = 0;
}

void StreamRing_ResetAll(void)
{
    for (unsigned v = 0; v < SAMPLE_VOICES; v++)
        StreamRing_Reset(v);
}

void StreamRing_Init(void)
{
    s_last_body_sequence = 0;
    s_audio_frames = s_last_body_frame = 0;
    s_have_body = 0;
    StreamRing_ResetAll();
}

/* ---- manage current and pending sessions -------------------------------- */

void StreamRing_Prime(uint8_t v)
{
    if (v < SAMPLE_VOICES) rings[v].consuming = 1;
}

void StreamRing_ArmPending(uint8_t v, uint16_t wave, uint8_t session)
{
    if (v >= SAMPLE_VOICES) return;
    Ring *r = &rings[v];
    initialize(r);
    reclaim(r);
    unsigned n = 0;
    while (n < QUEUES && r->queue[n].allocated)
        ++n;
    if (n == QUEUES) __builtin_trap();
    Queue *q = &r->queue[n];
    q->allocated = 1;
    ++q->generation;
    q->consumed = q->completed = q->reclaimed = 0;
    q->skip = 0;
    q->head = q->tail = q->reclaim = (uint16_t)allocate_node(r);
    q->dummy = 1;
    q->wave = wave;
    q->session = session;
    publish(&q->published, 0);
    publish(&q->position, 512u | q->head);
    uint32_t s = load(&r->state), old;
    do {
        old = s;
    } while (!change(r, &s, current(s), n));
    retire(r, pending(old));
}

int StreamRing_StartNote(uint8_t v)
{
    if (v >= SAMPLE_VOICES) return -1;
    Ring *r = &rings[v];
    uint32_t s = load(&r->state), old;
    do {
        old = s;
        if (pending(s) >= QUEUES) return -1;
    } while (!change(r, &s, pending(s), NONE));
    retire(r, current(old));
    r->consuming = 1;
    return 0;
}

void StreamRing_DiscardPending(uint8_t v)
{
    if (v >= SAMPLE_VOICES) return;
    Ring *r = &rings[v];
    uint32_t s = load(&r->state), old;
    do {
        old = s;
    } while (!change(r, &s, current(s), NONE));
    retire(r, pending(old));
}

void StreamRing_EndCurrent(uint8_t v)
{
    if (v >= SAMPLE_VOICES) return;
    Ring *r = &rings[v];
    uint32_t s = load(&r->state), old;
    do {
        old = s;
    } while (!change(r, &s, NONE, pending(s)));
    retire(r, current(old));
    r->consuming = 0;
}

void StreamRing_Release(uint8_t v)
{
    StreamRing_Reset(v);
}

/* ---- query ring sessions and capacity ----------------------------------- */

uint8_t StreamRing_CurrentSession(uint8_t v)
{
    if (v >= SAMPLE_VOICES) return NONE;
    Ring *r = &rings[v];
    unsigned i = current(load(&r->state));
    return i < QUEUES ? r->queue[i].session : NONE;
}

static uint32_t fill(Queue *q)
{
    return (uint16_t)((load(&q->published) >> 16) - q->consumed);
}

uint32_t StreamRing_CurrentFill(uint8_t v)
{
    if (v >= SAMPLE_VOICES) return 0;
    Ring *r = &rings[v];
    unsigned i = current(load(&r->state));
    return i < QUEUES ? fill(&r->queue[i]) : 0;
}

uint32_t StreamRing_PendingFill(uint8_t v)
{
    if (v >= SAMPLE_VOICES) return 0;
    Ring *r = &rings[v];
    unsigned i = pending(load(&r->state));
    return i < QUEUES ? fill(&r->queue[i]) : 0;
}

uint32_t StreamRing_FillLevel(uint8_t v)
{
    return StreamRing_CurrentFill(v);
}

uint32_t StreamRing_FreeLevel(uint8_t v)
{
    if (v >= SAMPLE_VOICES) return 0;
    Ring *r = &rings[v];
    uint32_t count = r->free_count, retired = load(&r->retired);
    for (unsigned i = 0; i < QUEUES; i++) {
        Queue *q = &r->queue[i];
        if (!q->allocated) continue;
        uint16_t available =
            (uint16_t)((retired & (1u << i)) ? load(&q->published) : (load(&q->position) >> 16));
        count += (uint16_t)(available - q->reclaimed);
    }
    return count * BLOCK;
}

uint32_t StreamRing_CurrentFree(uint8_t v)
{
    return StreamRing_FreeLevel(v);
}

uint8_t StreamRing_HasPending(uint8_t v)
{
    return v < SAMPLE_VOICES && pending(load(&rings[v].state)) < QUEUES;
}

uint8_t StreamRing_TargetSession(uint8_t v)
{
    if (v >= SAMPLE_VOICES) return NONE;
    Ring *r = &rings[v];
    uint32_t s = load(&r->state);
    unsigned i = pending(s);
    if (i >= QUEUES) i = current(s);
    return i < QUEUES ? r->queue[i].session : NONE;
}

uint32_t StreamRing_TargetFill(uint8_t v)
{
    return StreamRing_HasPending(v) ? StreamRing_PendingFill(v) : StreamRing_CurrentFill(v);
}

/* ---- reserve an incoming body block ------------------------------------- */

int StreamRing_WriteBegin(uint8_t v, uint8_t session, uint8_t sof, uint16_t wave, uint32_t n,
                          StreamRing_Write_t *w)
{
    if (!w) return STREAM_RING_WRITE_ERROR;
    memset(w, 0, sizeof *w);
    if (v >= SAMPLE_VOICES || !n || n > USB_STREAM_NSAMP_MAX) return STREAM_RING_WRITE_ERROR;
    Ring *r = &rings[v];
    reclaim(r);
    uint32_t s = load(&r->state);
    unsigned qi = pending(s);
    if (qi >= QUEUES || r->queue[qi].wave != wave ||
        (r->queue[qi].session != session && !(sof && r->queue[qi].session == NONE)))
        qi = current(s);
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
    w->pending = qi == pending(s);
    w->sof = sof;
    w->nsamp = n;
    w->active = 1;
    unsigned left = n;
    w->head = w->tail = w->cursor = NO_NODE;
    for (unsigned i = 0; i < blocks; i++) {
        unsigned node = allocate_node(r), b = r->free_head;
        r->free_head = (uint8_t)r->data[b * BLOCK];
        --r->free_count;
        r->block[node] = (uint8_t)b;
        unsigned length = left > BLOCK ? BLOCK : left;
        left -= length;
        r->info[node] = (uint16_t)((length << 9) | NO_NODE);
        if (w->head == NO_NODE)
            w->head = w->cursor = (uint16_t)node;
        else
            set_next(r, w->tail, node);
        w->tail = (uint16_t)node;
    }
    return STREAM_RING_WRITE_OK;
}

uint8_t StreamRing_WriteIsCurrent(const StreamRing_Write_t *w)
{
    if (!w || !w->active || w->voice >= SAMPLE_VOICES || w->bank >= QUEUES) return 0;
    Ring *r = &rings[w->voice];
    uint32_t s = load(&r->state);
    Queue *q = &r->queue[w->bank];
    return (current(s) == w->bank || pending(s) == w->bank) && q->generation == w->generation &&
           q->session == w->session;
}

int8_t *StreamRing_WriteSpan(StreamRing_Write_t *w, uint32_t *n)
{
    if (n) *n = 0;
    if (!w || !w->active || w->cursor == NO_NODE || w->written >= w->nsamp) return NULL;
    Ring *r = &rings[w->voice];
    if (n) *n = node_length(r, w->cursor) - w->offset;
    return &r->data[r->block[w->cursor] * BLOCK + w->offset];
}

int StreamRing_WriteAdvance(StreamRing_Write_t *w, uint32_t n)
{
    if (!w || !w->active || w->cursor == NO_NODE || !n) return -1;
    Ring *r = &rings[w->voice];
    if (n > (uint32_t)(node_length(r, w->cursor) - w->offset)) return -1;
    w->written += n;
    w->offset += (uint8_t)n;
    if (w->offset == node_length(r, w->cursor)) {
        w->cursor = (uint16_t)next_node(r, w->cursor);
        w->offset = 0;
    }
    return 0;
}

void StreamRing_WriteAbort(StreamRing_Write_t *w)
{
    if (!w || !w->active) return;
    Ring *r = &rings[w->voice];
    unsigned n = w->head;
    while (n != NO_NODE) {
        unsigned next = next_node(r, n);
        free_data(r, NULL, n);
        free_node(r, n);
        n = next;
    }
    w->active = 0;
}

/* ---- commit received body samples --------------------------------------- */

uint32_t StreamRing_WriteCommit(StreamRing_Write_t *w)
{
    if (!w || !w->active || w->written != w->nsamp) return 0;
    Ring *r = &rings[w->voice];
    uint8_t zero = 1;
    for (unsigned n = w->head; n != NO_NODE; n = next_node(r, n))
        for (unsigned i = 0; i < node_length(r, n); i++)
            if (r->data[r->block[n] * BLOCK + i]) zero = 0;
    if (!StreamRing_WriteIsCurrent(w)) {
        StreamRing_WriteAbort(w);
        return 0;
    }
    Queue *q = &r->queue[w->bank];
    uint32_t pub = load(&q->published);
    uint16_t wr = (uint16_t)pub, total = (uint16_t)(pub >> 16);
    set_next(r, q->tail, w->head);
    q->tail = w->tail;
    wr = (uint16_t)(wr + (w->nsamp + BLOCK - 1) / BLOCK);
    w->active = 0;
    publish(&q->published, ((uint32_t)(uint16_t)(total + w->nsamp) << 16) | wr);
    ++s_rx_pkts;
    if (zero) ++s_zero_pkts;
    if (w->sof) ++s_sof_pkts;
    return w->nsamp;
}

/* ---- write a complete body block ---------------------------------------- */

uint32_t StreamRing_WriteVoice(uint8_t v, uint8_t session, uint8_t sof, uint16_t wave,
                               const int8_t *s, uint32_t n)
{
    if (!s) return 0;
    StreamRing_Write_t w;
    if (StreamRing_WriteBegin(v, session, sof, wave, n, &w) != 0) return 0;
    while (w.written < n) {
        uint32_t span;
        int8_t *d = StreamRing_WriteSpan(&w, &span);
        if (!d || !span) {
            StreamRing_WriteAbort(&w);
            return 0;
        }
        memcpy(d, s + w.written, span);
        if (StreamRing_WriteAdvance(&w, span)) {
            StreamRing_WriteAbort(&w);
            return 0;
        }
    }
    return StreamRing_WriteCommit(&w);
}

int StreamRing_WriteBody(uint8_t v, uint8_t session, const int8_t *s, uint16_t n)
{
    if (v >= SAMPLE_VOICES || !s || !n || n > USB_STREAM_PAYLOAD_MAX) return -1;
    Ring *r = &rings[v];
    uint32_t state = load(&r->state);
    unsigned qi = pending(state);
    if (qi >= QUEUES || r->queue[qi].session != session) qi = current(state);
    if (qi >= QUEUES || r->queue[qi].session != session) {
        ++s_stale_pkts;
        ++s_last_body_sequence;
        return 0;
    }
    Queue *q = &r->queue[qi];
    if (((n + BLOCK - 1) / BLOCK) * BLOCK > StreamRing_FreeLevel(v)) return -1;
    unsigned accepted =
        StreamRing_WriteVoice(v, session, qi == pending(state) && fill(q) == 0, q->wave, s, n);
    s_last_body_frame = s_audio_frames;
    s_have_body = 1;
    ++s_last_body_sequence;
    return accepted == n ? 1 : 0;
}

/* ---- consume samples from the current ring ------------------------------ */

int StreamRing_Read(uint8_t v, uint32_t offset, int8_t *out, uint32_t count)
{
    if (v >= SAMPLE_VOICES || !out) return -1;
    Ring *r = &rings[v];
    unsigned qi = current(load(&r->state));
    if (qi >= QUEUES) return -1;
    Queue *q = &r->queue[qi];
    uint32_t available = fill(q);
    if (offset > available || count > available - offset) return -1;
    offset += q->skip;
    unsigned n = q->dummy ? next_node(r, q->head) : q->head;
    while (count) {
        uint32_t length = node_length(r, n);
        if (offset >= length) {
            offset -= length;
            n = next_node(r, n);
            continue;
        }
        uint32_t take = length - offset;
        if (take > count) take = count;
        for (uint32_t i = 0; i < take; i++)
            *out++ = r->data[r->block[n] * BLOCK + offset + i];
        count -= take;
        offset = 0;
        n = next_node(r, n);
    }
    return 0;
}

uint32_t StreamRing_Consume(uint8_t v, int8_t *out, uint32_t count)
{
    if (v >= SAMPLE_VOICES || !count) return 0;
    Ring *r = &rings[v];
    unsigned qi = current(load(&r->state));
    if (qi >= QUEUES) return 0;
    Queue *q = &r->queue[qi];
    uint32_t available = fill(q);
    if (count > available) count = available;
    uint32_t consumed = count;
    unsigned changed = 0;
    q->consumed += (uint16_t)count;
    while (count) {
        if (q->dummy) {
            q->head = (uint16_t)next_node(r, q->head);
            q->dummy = 0;
            changed = 1;
        }
        unsigned n = q->head;
        uint32_t length = node_length(r, n) - q->skip;
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
        publish(&q->position, ((uint32_t)q->completed << 16) | (q->dummy ? 512u : 0u) | q->head);
    if (r->consuming && available - consumed < s_min_fill) s_min_fill = available - consumed;
    return consumed;
}

int StreamRing_GetRel(uint8_t v, uint32_t offset, int8_t *out)
{
    return StreamRing_Read(v, offset, out, 1);
}

void StreamRing_Advance(uint8_t v, uint32_t n)
{
    (void)StreamRing_Consume(v, NULL, n);
}

/* ---- query and clear ring statistics ------------------------------------ */

uint32_t StreamRing_MaxFill(void)
{
    uint32_t n = 0;
    for (unsigned v = 0; v < SAMPLE_VOICES; v++) {
        uint32_t f = StreamRing_CurrentFill(v);
        if (f > n) n = f;
    }
    return n;
}

void StreamRing_ObserveFill(uint8_t v)
{
    if (v < SAMPLE_VOICES && rings[v].consuming) {
        uint32_t f = StreamRing_CurrentFill(v);
        if (f < s_min_fill) s_min_fill = f;
    }
}

uint16_t StreamRing_LastBodySequence(void)
{
    return s_last_body_sequence;
}

uint32_t StreamRing_MinFill(void)
{
    return s_min_fill;
}

uint32_t StreamRing_DropCount(void)
{
    return s_drop_pkts;
}

uint32_t StreamRing_RxCount(void)
{
    return s_rx_pkts;
}

uint32_t StreamRing_SofCount(void)
{
    return s_sof_pkts;
}

uint32_t StreamRing_ZeroCount(void)
{
    return s_zero_pkts;
}

uint32_t StreamRing_StaleCount(void)
{
    return s_stale_pkts;
}

uint32_t StreamRing_FutureCount(void)
{
    return s_future_pkts;
}

uint32_t StreamRing_FullCount(void)
{
    return s_full_pkts;
}

uint32_t StreamRing_SupersededCount(void)
{
    return s_superseded_pkts;
}

void StreamRing_StatsClear(void)
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

void StreamRing_DropCountClear(void)
{
    StreamRing_StatsClear();
}

void StreamRing_AudioFrame(void)
{
    ++s_audio_frames;
}

uint8_t StreamRing_BodyAgeMs(void)
{
    uint32_t frames = s_audio_frames - s_last_body_frame;
    if (s_have_body == 0u || frames >= 254u * 48u) return 255u;
    return (uint8_t)((frames + 47u) / 48u);
}
