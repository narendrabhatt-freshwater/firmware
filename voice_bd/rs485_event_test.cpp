#include "voicebd.cpp"

#include <cstring>
#include <iostream>
#include <stdexcept>

struct sp_port {};
static std::vector<std::string> queued_writes;
static std::vector<std::string> received_commands;
static unsigned purge_count;
static bool fail_write;

static void require(bool condition, char const* message)
{
    if (!condition) throw std::runtime_error(message);
}

extern "C" {
#if defined(__APPLE__)
int ioctl(int, unsigned long, ...) { return 0; }
#endif
sp_return sp_get_port_by_name(char const*, sp_port** port)
{ *port = new sp_port; return SP_OK; }
void sp_free_port(sp_port* port) { delete port; }
sp_return sp_open(sp_port*, sp_mode) { return SP_OK; }
sp_return sp_close(sp_port*) { return SP_OK; }
sp_return sp_set_baudrate(sp_port*, int) { return SP_OK; }
sp_return sp_set_bits(sp_port*, int) { return SP_OK; }
sp_return sp_set_stopbits(sp_port*, int) { return SP_OK; }
sp_return sp_set_parity(sp_port*, sp_parity) { return SP_OK; }
sp_return sp_set_flowcontrol(sp_port*, sp_flowcontrol) { return SP_OK; }
sp_return sp_get_port_handle(sp_port const*, void* handle)
{ *static_cast<int*>(handle) = -1; return SP_OK; }
sp_return sp_flush(sp_port*, sp_buffer)
{
    ++purge_count;
    queued_writes.clear();
    return SP_OK;
}
sp_return sp_blocking_write(sp_port*, void const* data, size_t size, unsigned)
{
    if (fail_write) return SP_ERR_ARG;
    queued_writes.emplace_back(static_cast<char const*>(data), size);
    return static_cast<sp_return>(size);
}
sp_return sp_blocking_read_next(sp_port*, void* data, size_t capacity, unsigned)
{
    require(!queued_writes.empty(), "read without a queued command");
    std::string command = queued_writes.back();
    received_commands.insert(received_commands.end(), queued_writes.begin(), queued_writes.end());
    queued_writes.clear();
    if (command == "c:vq\r") {
        uint8_t frame[61] = {0xa5, 0x5a, 0x43, 0x0e};
        frame[6] = 0xf0; frame[7] = 0x0f; 
        frame[60] = '\n';
        require(capacity >= sizeof frame, "status buffer too small");
        std::memcpy(data, frame, sizeof frame);
        return static_cast<sp_return>(sizeof frame);
    }
    char const reply[] = "[C] ok\r\n";
    require(capacity >= sizeof reply - 1, "reply buffer too small");
    std::memcpy(data, reply, sizeof reply - 1);
    return static_cast<sp_return>(sizeof reply - 1);
}
}

int main()
try {
    voice_board_detail::rs485_link link;
    require(bool(link.open("/dev/null", 921600)), "open failed");
    unsigned const setup_purges = purge_count;
    require(setup_purges != 0, "connection setup should discard stale input");
    for (unsigned round = 0; round < 100; ++round) {
        std::vector<std::string> expected;
        for (unsigned voice = 0; voice < 8; ++voice) {
            std::string body = "n" + std::to_string(voice) + " on 0 60 100 @1";
            require(bool(link.send_event(body)), "note-on write failed");
            expected.push_back("c:" + body + "\r");
        }
        require(received_commands.empty(), "note-on waited for a reply");
        require(bool(link.command("vq")), "status query failed");
        expected.push_back("c:vq\r");
        require(received_commands == expected, "status query lost queued note-ons");
        received_commands.clear();
        expected.clear();
        for (unsigned voice = 0; voice < 8; ++voice) {
            std::string body = "n" + std::to_string(voice) + " off";
            require(bool(link.send_event(body)), "note-off write failed");
            expected.push_back("c:" + body + "\r");
        }
        require(received_commands.empty(), "note-off waited for a reply");
        require(bool(link.command("g 1 18")), "control query failed");
        expected.push_back("c:g 1 18\r");
        require(received_commands == expected, "control query lost queued note-offs");
        received_commands.clear();
    }
    require(purge_count == setup_purges, "purged a live serial connection");
    fail_write = true;
    require(!link.send_event("n0 off"), "write failure was hidden");
    require(queued_writes.empty(), "failed write queued an event");
    std::cout << "PASS: queued note events survive status/control queries; write failures reported\n";
} catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
