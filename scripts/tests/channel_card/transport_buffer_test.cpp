#include "published_snapshot.h"
#include <atomic>
#include "transport_buffer.h"
#include <assert.h>
#include <thread>
#include <vector>
#include <deque>

int main()
{
    TransportByteQueue<8> queue;
    std::deque<uint8_t> expected;
    uint32_t random = 12345;
    for (unsigned turn = 0; turn < 100000; ++turn) {
        random = random * 1664525u + 1013904223u;
        uint8_t bytes[11], out[11];
        unsigned count = (random >> 16) % 11;
        for (unsigned i = 0; i < count; ++i) bytes[i] = (uint8_t)(turn + i);
        if (random & 1) {
            bool fits = count <= 8 - expected.size();
            assert(queue.write(bytes, count) == fits);
            if (fits) for (unsigned i = 0; i < count; ++i) expected.push_back(bytes[i]);
        } else {
            unsigned want = count < expected.size() ? count : expected.size();
            assert(queue.peek(out, count) == want);
            for (unsigned i = 0; i < want; ++i) assert(out[i] == expected[i]);
            assert(queue.read(out, count) == want);
            for (unsigned i = 0; i < want; ++i) {
                assert(out[i] == expected.front()); expected.pop_front();
            }
        }
        assert(queue.size() == expected.size());
        assert(queue.free() + queue.size() == 8);
    }
    TransportByteQueue<8192, 8191> uart;
    std::vector<uint8_t> full(8192, 42);
    assert(uart.write(full.data(), 8191));
    assert(!uart.write(full.data(), 1));
    uart.discard(); assert(uart.free() == 8191);

    UsbTransport usb;
    uint8_t data[] = {1, 2, 3}, out[3];
    assert(!usb.ready() && !usb.write(data, 3));
    usb.configured = usb.attached = 1;
    assert(usb.ready() && usb.write(data, 3));
    assert(usb.rx.write(data, 3));
    usb.suspended = 1;
    assert(!usb.ready() && usb.generation == 0 && usb.rx.size() == 3);
    usb.suspended = 0;
    usb.fault();
    assert(!usb.ready() && usb.read(out, 3) == 0);
    assert(usb.tx.read(out, 3) == 3); // queued error/reply can still drain
    usb.reset();
    assert(usb.ready() && usb.generation == 1);
    assert(!usb.rx.size() && !usb.tx.size());
    usb.fault(true); assert(usb.generation == 2 && !usb.ready());

    TransportByteQueue<256> concurrent;
    constexpr unsigned total = 1000000;
    std::thread producer([&] {
        for (unsigned i = 0; i < total; ++i) {
            uint8_t byte = (uint8_t)i;
            while (!concurrent.write(&byte, 1)) std::this_thread::yield();
        }
    });
    for (unsigned i = 0; i < total; ++i) {
        uint8_t byte;
        while (!concurrent.read(&byte, 1)) std::this_thread::yield();
        assert(byte == (uint8_t)i);
    }
    producer.join(); assert(!concurrent.size());
    struct Status { unsigned sequence, inverse, copies[32]; };
    PublishedSnapshot<Status> snapshot;
    std::atomic<bool> complete{false};
    std::thread publisher([&] {
        for (unsigned i=1; i<=total; ++i) {
            Status status{}; status.sequence=i; status.inverse=~i;
            for (auto &copy : status.copies) copy=i;
            snapshot.publish(status);
        }
        complete.store(true, std::memory_order_release);
    });
    unsigned last=0;
    do {
        const auto &status=snapshot.read();
        assert(status.sequence >= last);
        if (status.sequence) {
            assert(status.inverse == ~status.sequence);
            for (auto copy : status.copies) assert(copy == status.sequence);
        }
        last=status.sequence;
    } while (!complete.load(std::memory_order_acquire) || last != total);
    publisher.join();
}
