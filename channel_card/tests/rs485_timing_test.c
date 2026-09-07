#include "rs485_config.h"
#include <assert.h>

int main(void)
{
    const uint32_t rates[] = {9600u, 115200u, 921600u};
    /* A complete status frame and the largest Channel reply must fit. */
    assert(fw_rs485_tx_deadline_ms(56u, 115200u) >= 7u);
    assert(fw_rs485_tx_deadline_ms(223u, 115200u) >= 22u);
    for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
        for (uint32_t bytes = 1; bytes <= 223; ++bytes) {
            uint32_t deadline = fw_rs485_tx_deadline_ms(bytes, rates[r]);
            /* Retain at least two ms beyond physical 8N1 transmission. */
            assert((uint64_t)(deadline - 2u) * rates[r] >=
                   (uint64_t)bytes * 10000u);
            /* Avoid long blocking waits that prevent USB audio service. */
            assert((uint64_t)(deadline - 3u) * rates[r] <
                   (uint64_t)bytes * 10000u);
        }
    }
    return 0;
}
