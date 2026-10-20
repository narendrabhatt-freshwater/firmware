#include "../../../channel_card/app/board_stm32.cpp"
#include "../../../channel/serial_buffer.cpp"
#include "../../../channel/transport.cpp"
#include <cassert>
#include <cstring>

static unsigned wakes;
void signal_interrupt() { ++wakes; }
void request_main_loop() { ++wakes; }
void set_platform_synchronization(const PlatformSynchronization *) {}
void set_main_loop_wakeup(void (*)()) {}
void set_channel_outputs(const ChannelOutputs *) {}
HAL_StatusTypeDef set_cs4304_channel_trim(CS4304_HandleTypeDef *, char, uint8_t) { return HAL_OK; }
[[noreturn]] void Error_Handler() { std::abort(); }

static void complete_wire()
{
    // FIFO empty is not wire completion. TC is the final stop bit.
    uart.ISR = USART_ISR_TXE_TXFNF;
    UART5_IRQHandler();
    assert(!tx_left && tx_active && !tx_complete);
    assert(control_gpio.state == GPIO_PIN_SET);
    uart.ISR = USART_ISR_TC;
    UART5_IRQHandler();
    uart.ISR = 0;
    assert(tx_complete);
}
static void queue_reply()
{
    const uint8_t reply[] = {1,2,3,4};
    assert(queue_rs485_reply(reply, sizeof reply, nullptr) == 0);
}
int main()
{
    rs485_serial_buffer.process = process_rs485_replies;
    queue_reply(); process_rs485();
    assert(tx_active && rs485_serial_buffer.output_size == 4);
    assert(control_gpio.state == GPIO_PIN_SET);
    process_rs485(); assert(has_pending_rs485_reply());
    complete_wire(); process_rs485();
    assert(!has_pending_rs485_reply() && !rs485_serial_buffer.transmit_failed);
    assert(control_gpio.state == GPIO_PIN_RESET);

    queue_reply(); process_rs485();
    now_ms += 2; process_rs485(); assert(tx_active);
    now_ms += 1; process_rs485();
    assert(!tx_active && rs485_serial_buffer.transmit_failed && !has_pending_rs485_reply());

    now_ms = 0xfffffffeu;
    queue_reply(); process_rs485();
    now_ms += 3; process_rs485();
    assert(!has_pending_rs485_reply() && rs485_serial_buffer.transmit_failed);

    control_gpio.state = GPIO_PIN_SET;
    queue_reply(); process_rs485();
    assert(!tx_active && waiting_for_bus && has_pending_rs485_reply());
    now_ms += 249; process_rs485(); assert(has_pending_rs485_reply());
    now_ms += 1; process_rs485();
    assert(!has_pending_rs485_reply() && rs485_serial_buffer.transmit_failed);
    assert(control_gpio.state == GPIO_PIN_SET); // Never release another card's bus.
    control_gpio.state = GPIO_PIN_RESET;

    // The driver passes received bytes and lifetime drop counts to the app.
    rx_buf[0] = 'a'; rx_buf[1] = 'b'; rx_head = 2; rx_tail = 0; rx_dropped = 9;
    rs485_serial_buffer.process = +[] {
        assert(rs485_serial_buffer.input_size == 2);
        assert(!std::memcmp(rs485_serial_buffer.input, "ab", 2));
        assert(rs485_serial_buffer.dropped == 9);
        rs485_serial_buffer.clear_input = true;
    };
    process_rs485();
    assert(rx_tail == rx_head && !rs485_serial_buffer.input_size);
    assert(!rs485_serial_buffer.clear_input && rx_dropped == 9);
    assert(wakes);
}
