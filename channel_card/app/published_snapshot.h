#ifndef CHANNEL_PUBLISHED_SNAPSHOT_H
#define CHANNEL_PUBLISHED_SNAPSHOT_H
#include <stdint.h>

/* One publishing owner, one reading owner. Both operations are bounded; neither
 * retries or waits. The reader owns its slot until its next read. Writers never
 * touch that slot. Only the newest completed publication needs to be observed. */
template<class T> class PublishedSnapshot {
    static_assert(__atomic_always_lock_free(4, 0), "snapshot publication must not block");
    T slots_[3]{};
    uint32_t middle_ = 1, writer_ = 2, reader_ = 0;
public:
    void publish(const T &value) {
        slots_[writer_] = value;
        writer_ = __atomic_exchange_n(&middle_, writer_ | 4u, __ATOMIC_ACQ_REL) & 3u;
    }
    const T &read() {
        if (__atomic_load_n(&middle_, __ATOMIC_ACQUIRE) & 4u)
            reader_ = __atomic_exchange_n(&middle_, reader_, __ATOMIC_ACQ_REL) & 3u;
        return slots_[reader_];
    }
};
#endif
