// Real PTY backpressure: no board or implicit production timeout required.
#include "voicebd.cpp"
#include <cassert>
#include <iostream>
using namespace voice_board_detail;

struct Pty {
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    std::string slave;
    Pty() {
        assert(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
        slave = ptsname(master);
    }
    void fill() {
        int fd = ::open(slave.c_str(), O_WRONLY | O_NOCTTY | O_NONBLOCK);
        assert(fd >= 0);
        char bytes[4096]{};
        while (::write(fd, bytes, sizeof bytes) > 0) {}
        assert(errno == EAGAIN || errno == EWOULDBLOCK);
        ::close(fd);
    }
    ~Pty() { ::close(master); }
};

int main()
{
    Pty pty;
    rs485_link link;
    assert(link.open(pty.slave, 921600));
    pty.fill();
    auto pending = std::async(std::launch::async, [&] { return link.send_event("clear"); });
    // Explicit observation windows belong only to this test.
    assert(pending.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);
    link.cancel();
    if (pending.wait_for(std::chrono::seconds(2)) != std::future_status::ready) std::_Exit(1);
    auto result = pending.get();
    assert(!result && result.message.find("cancelled") != std::string::npos);
    assert(!link.send_event("clear")); // Cancellation stays latched until reopen.
    link.clear_on_close(); // Full queue must not stall shutdown.
    link.close();
    assert(link.open(pty.slave, 921600, 0, 5));
    pty.fill();
    result = link.send_event("clear");
    assert(!result && result.message.find("timed out") != std::string::npos);
    link.close();

    // Reopen clears cancellation. A slow reader forces partial writes; every
    // byte must arrive once, in order, with default unlimited write expiry.
    serial_port port;
    std::string error;
    assert(port.open(pty.slave, 921600, false, error));
    std::string expected(65536, 'x');
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = char(i % 127);
    auto writer = std::async(std::launch::async, [&] { return port.write_all(expected.data(), expected.size(), error); });
    std::string received;
    auto watchdog = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (received.size() < expected.size()) {
        if (std::chrono::steady_clock::now() >= watchdog) std::_Exit(2);
        pollfd fd{pty.master, POLLIN, 0};
        if (::poll(&fd, 1, 10) <= 0) continue;
        char bytes[113];
        auto n = ::read(pty.master, bytes, sizeof bytes);
        if (n > 0) received.append(bytes, size_t(n));
    }
    assert(writer.get() && received == expected);
    port.close();
    assert(link.open(pty.slave, 921600));
    assert(link.send_event("clear"));
    std::cout << "PASS full-queue cancellation, explicit expiry, partial writes and reopen\n";
}
