/* Host implementation of the Channel Card hardware layer.
 * Replaces board_stm32.cpp, usb_device.cpp and audio_i2s.cpp. Queue sizes and
 * acceptance rules follow those files so the shared code sees the same
 * backpressure; electrical behaviour and interrupt preemption are not modeled. */

#include "sim.h"
#include "audio.h"
#include "board.h"
#include "channel.h"
#include "usb.h"
#include <stdio.h>
#include <string.h>
#include <deque>

/* ---- machine state ------------------------------------------------------ */

static SimCounters s_counters;
static uint64_t s_refills;
static uint8_t s_leds[BOARD_LED_COUNT];
static uint8_t s_switches[BOARD_SWITCH_COUNT];
static int s_dac_trim[4] = {-1, -1, -1, -1};
static uint8_t s_cpu_load_probe;

/* ---- read time and mask interrupts -------------------------------------- */

/* The audio clock is the only clock: one tick per DMA refill interrupt. */
uint32_t Board_Millis(void)
{
    return (uint32_t)s_refills;
}

uint32_t Sim_Millis(void)
{
    return Board_Millis();
}

/* Nothing preempts on this single thread, so there is nothing to mask. */
uint32_t Board_IrqSave(void)
{
    return 0u;
}

void Board_IrqRestore(uint32_t saved)
{
    (void)saved;
}

/* ---- drive leds and analog switches ------------------------------------- */

void Board_LedWrite(uint8_t led, uint8_t on)
{
    if (led < BOARD_LED_COUNT) s_leds[led] = on != 0u;
}

void Board_SwitchWrite(uint8_t sw, uint8_t high)
{
    if (sw < BOARD_SWITCH_COUNT) s_switches[sw] = high != 0u;
}

uint8_t Sim_Led(uint8_t led)
{
    return led < BOARD_LED_COUNT ? s_leds[led] : 0u;
}

uint8_t Sim_Switch(uint8_t sw)
{
    return sw < BOARD_SWITCH_COUNT ? s_switches[sw] : 0u;
}

/* ---- set dac attenuation ------------------------------------------------ */

uint8_t Board_DacReady(void)
{
    return 1u;
}

void Board_DacSetTrim(char channel, uint8_t trim)
{
    if (channel >= '1' && channel <= '4') s_dac_trim[channel - '1'] = trim;
}

int Sim_DacTrim(uint8_t channel)
{
    return channel >= 1u && channel <= 4u ? s_dac_trim[channel - 1u] : -1;
}

/* ---- rs485 port --------------------------------------------------------- */

/* Same receive ring size as the UART5 driver: a full ring drops new bytes. */
#define UART_RX_SIZE 8192u
#define UART_RX_MASK (UART_RX_SIZE - 1u)

static uint8_t uart_rx[UART_RX_SIZE];
static uint16_t uart_head, uart_tail;
static uint32_t uart_dropped;
static std::deque<uint8_t> uart_tx;
static uint8_t rs485_driving;
static uint32_t rs485_bus_errors;

void Uart5Rx_Init(void)
{
    uart_head = uart_tail = 0u;
    uart_dropped = 0u;
}

uint8_t Uart5Rx_Get(uint8_t *out)
{
    if (out == NULL || uart_tail == uart_head) return 0u;
    *out = uart_rx[uart_tail];
    uart_tail = (uint16_t)((uart_tail + 1u) & UART_RX_MASK);
    return 1u;
}

void Uart5Rx_Clear(void)
{
    uart_tail = uart_head;
}

uint32_t Uart5Rx_DroppedCount(void)
{
    return uart_dropped;
}

/* The bytes are on the wire at once; baud-rate pacing is not modeled. */
int Uart5Rx_Transmit(const uint8_t *data, uint16_t size, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (data == NULL || size == 0u) return -1;
    if (!rs485_driving) rs485_bus_errors++;
    uart_tx.insert(uart_tx.end(), data, data + size);
    return 0;
}

void Board_Rs485Release(void)
{
    rs485_driving = 0u;
}

void Board_Rs485Acquire(void)
{
    rs485_driving = 1u;
}

uint8_t Board_Rs485BusFree(void)
{
    return 1u;
}

uint32_t Board_Rs485Baud(void)
{
    return FW_RS485_BAUD;
}

uint32_t Sim_Rs485BusErrors(void)
{
    return rs485_bus_errors;
}

void Sim_Rs485HostWrite(const uint8_t *data, size_t size)
{
    if (size) s_counters.rs485_events++;
    for (size_t i = 0; i < size; i++) {
        uint16_t next = (uint16_t)((uart_head + 1u) & UART_RX_MASK);
        if (next == uart_tail) {
            uart_dropped++;
        } else {
            uart_rx[uart_head] = data[i];
            uart_head = next;
        }
    }
}

size_t Sim_Rs485HostRead(uint8_t *data, size_t size)
{
    size_t n = 0;
    while (n < size && !uart_tx.empty()) {
        data[n++] = uart_tx.front();
        uart_tx.pop_front();
    }
    return n;
}

/* ---- usb cdc port ------------------------------------------------------- */

/* Same queue sizes as usb_device.cpp. OUT is armed only while a whole
 * 512-byte transfer fits; reads hand the parser at most 64 bytes. */
#define USB_RX_SIZE 8192u
#define USB_TX_SIZE 2048u
#define USB_RX_TRANSFER_BYTES 512u

static uint8_t usb_rx[USB_RX_SIZE], usb_tx[USB_TX_SIZE];
static uint32_t usb_rx_wr, usb_rx_rd, usb_tx_wr, usb_tx_rd, usb_epoch;
static uint8_t usb_attached, usb_faulted;

static void usb_reset_queues(void)
{
    usb_rx_wr = usb_rx_rd = usb_tx_wr = usb_tx_rd = 0u;
    usb_faulted = 0u;
    ++usb_epoch;
}

void USB_Device_Init(void)
{
    usb_attached = 0u;
    usb_reset_queues();
}

uint32_t USB_Device_Read(uint8_t *dst, uint32_t size)
{
    if (usb_faulted) return 0u;
    uint32_t n = usb_rx_wr - usb_rx_rd;
    if (n > size) n = size;
    if (n > 64u) n = 64u;
    for (uint32_t i = 0; i < n; i++)
        dst[i] = usb_rx[(usb_rx_rd + i) % USB_RX_SIZE];
    usb_rx_rd += n;
    return n;
}

uint8_t USB_Device_Write(const uint8_t *src, uint32_t size)
{
    if (!usb_attached || size > USB_TX_SIZE - (usb_tx_wr - usb_tx_rd)) return 0u;
    for (uint32_t i = 0; i < size; i++)
        usb_tx[(usb_tx_wr + i) % USB_TX_SIZE] = src[i];
    usb_tx_wr += size;
    return 1u;
}

uint32_t USB_Device_TxFree(void)
{
    return USB_TX_SIZE - (usb_tx_wr - usb_tx_rd);
}

uint32_t USB_Device_Epoch(void)
{
    return usb_epoch;
}

uint8_t USB_Device_Connected(void)
{
    return (uint8_t)(usb_attached && !usb_faulted);
}

void USB_Device_Fault(void)
{
    usb_faulted = 1u;
}

void USB_Device_Debug(char *text, uint32_t size)
{
    (void)snprintf(text, size, "ok: usbdev simulated attached %u fault %u epoch %lu\r\n",
                   usb_attached, usb_faulted, (unsigned long)usb_epoch);
}

void Sim_UsbAttach(void)
{
    usb_attached = 1u;
    usb_reset_queues();
}

void Sim_UsbDetach(void)
{
    usb_attached = 0u;
    usb_reset_queues();
}

size_t Sim_UsbHostSpace(void)
{
    uint32_t space = USB_RX_SIZE - (usb_rx_wr - usb_rx_rd);
    if (!usb_attached || usb_faulted || space < USB_RX_TRANSFER_BYTES) return 0u;
    return space;
}

size_t Sim_UsbHostWrite(const uint8_t *data, size_t size)
{
    size_t done = 0;
    while (done < size) {
        size_t n = Sim_UsbHostSpace();
        if (n == 0u) break;
        if (n > USB_RX_TRANSFER_BYTES) n = USB_RX_TRANSFER_BYTES;
        if (n > size - done) n = size - done;
        for (size_t i = 0; i < n; i++)
            usb_rx[(usb_rx_wr + i) % USB_RX_SIZE] = data[done + i];
        usb_rx_wr += (uint32_t)n;
        done += n;
    }
    if (done) s_counters.usb_events++;
    return done;
}

size_t Sim_UsbHostRead(uint8_t *data, size_t size)
{
    size_t n = usb_tx_wr - usb_tx_rd;
    if (n > size) n = size;
    for (size_t i = 0; i < n; i++)
        data[i] = usb_tx[(usb_tx_rd + i) % USB_TX_SIZE];
    usb_tx_rd += (uint32_t)n;
    return n;
}

/* ---- audio clock -------------------------------------------------------- */

void Audio_Bridge_CpuLoadProbeSet(uint8_t enabled)
{
    s_cpu_load_probe = enabled != 0u ? 1u : 0u;
}

uint8_t Audio_Bridge_CpuLoadProbeGet(void)
{
    return s_cpu_load_probe;
}

/* The two circular DMA buffers of audio_i2s.cpp, same size and layout:
 * interleaved L+R words, two refills long. I2S1 carries CH1+CH2 and I2S2
 * carries CH3+CH4. The DAC reads frame after frame at `dma_frame`. When it
 * leaves the first half the "half complete" interrupt refills that half, and
 * when it wraps the "complete" interrupt refills the second half, so a refill
 * is heard one millisecond after it is rendered. */
#define DMA_HALF_FRAMES AUDIO_REFILL_FRAMES
#define DMA_BUF_FRAMES (DMA_HALF_FRAMES * 2u)

static int32_t i2s1_tx_buf[DMA_BUF_FRAMES * 2u], i2s2_tx_buf[DMA_BUF_FRAMES * 2u];
static uint32_t dma_frame;
static uint8_t i2s_started;

/* Same prefill as the card: silence on CH1 and slewing control voltages on
 * CH2..CH4 for the two milliseconds before the first refill is heard. */
void Audio_StartPlayback(void)
{
    if (i2s_started) return;
    memset(i2s1_tx_buf, 0, sizeof i2s1_tx_buf);
    memset(i2s2_tx_buf, 0, sizeof i2s2_tx_buf);
    Audio_FillDCSlot(i2s1_tx_buf, DMA_BUF_FRAMES, 1, 1);
    Audio_FillDC(i2s2_tx_buf, DMA_BUF_FRAMES, 2, 3);
    dma_frame = 0u;
    i2s_started = 1u;
}

/* The DMA half/complete interrupt: refill the half the DAC just left. The
 * main loop gets its turn first, as it runs between interrupts on the card. */
static void dma_interrupt(uint8_t half)
{
    uint32_t at = half * DMA_HALF_FRAMES * 2u;
    (void)Sim_RunMainLoop();
    Audio_RenderI2S1(&i2s1_tx_buf[at], DMA_HALF_FRAMES);
    Audio_FillDC(&i2s2_tx_buf[at], DMA_HALF_FRAMES, 2, 3);
    ++s_refills;
    s_counters.refills++;
}

void Sim_AudioRequest(int32_t *out, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; i++) {
        if (out != NULL) {
            out[i * SIM_AUDIO_CHANNELS + 0u] = i2s1_tx_buf[dma_frame * 2u];
            out[i * SIM_AUDIO_CHANNELS + 1u] = i2s1_tx_buf[dma_frame * 2u + 1u];
            out[i * SIM_AUDIO_CHANNELS + 2u] = i2s2_tx_buf[dma_frame * 2u];
            out[i * SIM_AUDIO_CHANNELS + 3u] = i2s2_tx_buf[dma_frame * 2u + 1u];
        }
        ++dma_frame;
        if (dma_frame == DMA_HALF_FRAMES) {
            dma_interrupt(0u);
        } else if (dma_frame == DMA_BUF_FRAMES) {
            dma_frame = 0u;
            dma_interrupt(1u);
        }
    }
}

/* ---- main loop ---------------------------------------------------------- */

uint32_t Sim_RunMainLoop(void)
{
    uint32_t worked = 0;
    for (;;) {
        uint32_t usb_rd = usb_rx_rd, usb_wr = usb_tx_wr, epoch = usb_epoch;
        uint16_t tail = uart_tail;
        size_t sent = uart_tx.size();
        ChannelApp_Step();
        if (usb_rd == usb_rx_rd && usb_wr == usb_tx_wr && epoch == usb_epoch && tail == uart_tail &&
            sent == uart_tx.size()) {
            s_counters.idle_passes++;
            return worked;
        }
        s_counters.main_passes++;
        ++worked;
    }
}

const SimCounters *Sim_Counters(void)
{
    return &s_counters;
}

/* ---- start the card ----------------------------------------------------- */

void Sim_Boot(void)
{
    USB_App_Init();
    Audio_SetDCLevel(2, 0);
    Audio_SetDCLevel(3, 0);
    Audio_SetDCLevel(4, 0);
    Audio_StartPlayback();
    /* The banner goes out as the console comes up; a host that connects
     * later never sees it, as with a card that was already running. */
    ChannelConsole_Init();
}
