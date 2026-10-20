#include "../../../channel_card/app/usb_device.cpp"
#include "../../../channel/serial_buffer.cpp"
#include <cassert>
#include <cstring>

static unsigned wakes, calls;
void request_main_loop() { ++wakes; }
[[noreturn]] void Error_Handler() { std::abort(); }
static void configure()
{
    HAL_PCD_ResetCallback(PCD);
    uint8_t setup[] = {0,9,1,0,0,0,0,0};
    std::memcpy(PCD->Setup, setup, 8); HAL_PCD_SetupStageCallback(PCD);
    setup[0] = 0x21; setup[1] = 0x22;
    std::memcpy(PCD->Setup, setup, 8); HAL_PCD_SetupStageCallback(PCD);
    process_usb();
    assert(usb_serial_buffer.connected && !usb_serial_buffer.faulted);
}
int main()
{
    init_usb_device();
    configure();
    assert(usb_serial_buffer.format_status);
    usb_serial_buffer.process = +[] { ++calls; };
    // A partial OUT transfer is visible before a full 512-byte completion.
    std::memcpy(out_packet, "hello", 5); usb_rx_count = 5;
    process_usb();
    assert(calls == 1 && usb_serial_buffer.input_size == 5);
    assert(!std::memcmp(usb_serial_buffer.input, "hello", 5));
    HAL_PCD_DataOutStageCallback(PCD, 1); process_usb();
    assert(usb_serial_buffer.input_size == 5); // No duplicate publication.

    uint32_t generation = usb_serial_buffer.generation;
    HAL_PCD_SuspendCallback(PCD); process_usb();
    assert(!usb_serial_buffer.connected && usb_serial_buffer.generation == generation);
    HAL_PCD_ResumeCallback(PCD); process_usb();
    assert(usb_serial_buffer.connected && usb_serial_buffer.generation == generation);

    usb_serial_buffer.process = +[] {
        const uint8_t response[] = {9,8,7};
        assert(queue_serial_output(usb_serial_buffer, response, sizeof response));
        usb_serial_buffer.faulted = true;
    };
    process_usb(); usb_serial_buffer.process = nullptr;
    assert(faulted && usb_serial_buffer.faulted);
    assert(usb_wire == std::vector<uint8_t>({9,8,7}));
    unsigned before = usb_serial_buffer.input_size;
    out_packet[0] = 42; usb_rx_count = 1;
    HAL_PCD_DataOutStageCallback(PCD, 1); process_usb();
    assert(usb_serial_buffer.input_size == before);

    configure();
    assert(!usb_serial_buffer.input_size && !usb_serial_buffer.output_size);
    assert(usb_serial_buffer.generation != generation);
    usb_wire.clear();
    // A reset during the application handler must discard its old-generation reply.
    usb_serial_buffer.process = +[] {
        const uint8_t stale = 55;
        assert(queue_serial_output(usb_serial_buffer, &stale, 1));
        HAL_PCD_DisconnectCallback(PCD);
    };
    process_usb(); usb_serial_buffer.process = nullptr;
    assert(usb_wire.empty() && wakes);
    process_usb();
    assert(!usb_serial_buffer.connected && !usb_serial_buffer.output_size);

    // Full USB TX queue retains application output until hardware makes room.
    configure();
    tx_busy = 1; tx_wr = TX_SIZE; tx_rd = 0;
    const uint8_t response = 99;
    assert(queue_serial_output(usb_serial_buffer, &response, 1));
    process_usb(); assert(usb_serial_buffer.output_size == 1);
    tx_rd = tx_wr; tx_busy = 0;
    process_usb();
    assert(!usb_serial_buffer.output_size && usb_wire.back() == 99);
}
