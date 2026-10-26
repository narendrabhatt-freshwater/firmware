#include "board.h"
#include "channel.h"
#include "hardware.h"

int32_t audio_buffers[BUF_COUNT][SPB];
volatile unsigned audio_write_index(0);
volatile bool audio_completed(false);

/* ---- run parsing and rendering on the main thread ------------------------ */

int main()
{
    std::vector<uint8_t> rs485_input;
    init_board();
    init_serial();
    init_dac();
    init_audio();
    for (unsigned i(0); i < BUF_COUNT; ++i) generate_samples(audio_buffers[i]);
    start_audio();
    for (;;) {
        if (process_serial(rs485_input)) {
            received_serial(rs485_input);
            rs485_input.clear();
        }
        if (audio_completed) {
            audio_completed = false;
            generate_samples(audio_buffers[audio_write_index]);
            __DMB();
            audio_write_index = (audio_write_index + 1) % BUF_COUNT;
        }
    }
}
