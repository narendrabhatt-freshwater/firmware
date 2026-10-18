#ifndef CHANNEL_TRANSPORT_BUFFER_H
#define CHANNEL_TRANSPORT_BUFFER_H

#include <stdint.h>
#include <string.h>

/* Fixed storage, one producer and one consumer. Indices publish ownership with
 * acquire/release ordering on both the MCU and host. Reset requires both ends
 * quiescent (the STM32 adapter holds its interrupt mask). No device policy here.
 * Capacity is a power of two so unsigned index rollover preserves addressing.
 */
template<uint32_t Capacity, uint32_t Limit = Capacity>
class TransportByteQueue {
    static_assert(Capacity && !(Capacity & (Capacity - 1)), "power-of-two capacity");
    static_assert(Limit && Limit <= Capacity, "valid usable capacity");
    static_assert(__atomic_always_lock_free(4, nullptr), "lock-free queue indices");
    uint8_t bytes_[Capacity]{};
    uint32_t written_ = 0, read_ = 0;
public:
    uint32_t size() const {
        return __atomic_load_n(&written_, __ATOMIC_ACQUIRE) -
               __atomic_load_n(&read_, __ATOMIC_ACQUIRE);
    }
    uint32_t free() const { return Limit - size(); }
    void reset() {
        __atomic_store_n(&written_, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&read_, 0u, __ATOMIC_RELAXED);
    }
    void discard() {
        __atomic_store_n(&read_, __atomic_load_n(&written_, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    }
    bool write(const uint8_t *data, uint32_t count) {
        if (count > free()) return false;
        if (!count) return true;
        uint32_t wr = __atomic_load_n(&written_, __ATOMIC_RELAXED);
        uint32_t at = wr & (Capacity - 1), first = count;
        if (first > Capacity - at) first = Capacity - at;
        memcpy(bytes_ + at, data, first);
        memcpy(bytes_, data + first, count - first);
        __atomic_store_n(&written_, wr + count, __ATOMIC_RELEASE);
        return true;
    }
    uint32_t peek(uint8_t *data, uint32_t count) const {
        uint32_t available = size();
        if (count > available) count = available;
        if (!count) return 0;
        uint32_t at = __atomic_load_n(&read_, __ATOMIC_RELAXED) & (Capacity - 1);
        uint32_t first = count;
        if (first > Capacity - at) first = Capacity - at;
        memcpy(data, bytes_ + at, first);
        memcpy(data + first, bytes_, count - first);
        return count;
    }
    /* Consumer borrows only the first contiguous span until consume/reset. */
    const uint8_t *front(uint32_t &count) const {
        count = size();
        uint32_t at = __atomic_load_n(&read_, __ATOMIC_RELAXED) & (Capacity - 1);
        if (count > Capacity - at) count = Capacity - at;
        return bytes_ + at;
    }
    void consume(uint32_t count) {
        uint32_t rd = __atomic_load_n(&read_, __ATOMIC_RELAXED);
        __atomic_store_n(&read_, rd + count, __ATOMIC_RELEASE);
    }
    uint32_t read(uint8_t *data, uint32_t count) {
        count = peek(data, count);
        consume(count);
        return count;
    }
};

struct TransportBuffers {
    TransportByteQueue<8192> rx;
    TransportByteQueue<2048> tx;
    void clear() { rx.reset(); tx.reset(); }
};

/* Shared USB byte-stream lifecycle. Device adapters translate configuration,
 * DTR, suspend and PTY events into this state under their own serialization.
 * Fault stops receive but permits an already queued error reply to drain.
 * Suspend changes readiness without discarding bytes or advancing generation.
 */
struct UsbTransport : TransportBuffers {
    volatile uint32_t generation = 0;
    volatile uint8_t configured = 0, attached = 0, suspended = 0, faulted = 0;
    void reset() {
        clear();
        faulted = 0;
        ++generation;
    }
    void fault(bool invalidate = false) {
        faulted = 1;
        if (invalidate) ++generation;
    }
    uint8_t ready() const { return configured && attached && !suspended && !faulted; }
    uint32_t read(uint8_t *out, uint32_t size) { return faulted ? 0 : rx.read(out, size); }
    uint8_t write(const uint8_t *data, uint32_t size) {
        return configured && attached && tx.write(data, size);
    }
};
#endif
