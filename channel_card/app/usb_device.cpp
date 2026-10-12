/*
                                   __
                               ___  \  \
                          ___  \  \  \  \    _______
                     ___  \  \  \  \  \__\__/_____  \
                     \  \  \  \  \__\_______      \__\____
                      \  \  \  \     ___ \  \________  \__/
                       \  \  \__\___/_  \ \___/   \  \___
                        \  \    ____  \  \________ \ ___/
                         \__\__/ \  \  \____/  \  \
                                  \  \     ___  \  \___
                                   \__\___/ \  \ \____/
                                           \  \
                                              \  \___
                 __                 _          \ ___/  _
                / _|_ __  ___  ___ | |____      ____ _| |_  ___  _ __
               | |_| '_ |/ _ \/ __|| '_ \ \ /\ / / _` | __|/ _ \| '__|
               |  _| |  |  __/\__ \| | | \ V  V / (_| | |_|  __/| |
               |_| |_|   \___||___/|_| |_|\_/\_/ \__,_|\__|\___||_|

            (C) 2 0 2 6   F r e s h w a t e r   I n s t r u m e n t s
*/

#include "transport.h"
#include "usb_device.h"
#include "usb.h"
#include "usb_otg.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

/* ---- handle usb device transfers ---------------------------------------- */

/* Minimal full-speed CDC ACM device on the existing STM32 HAL PCD.
 * IRQ owns endpoints. Queue publication uses short interrupt critical sections.
 * OUT accepts eight packets per transfer and is rearmed in its completion ISR.
 * Main-loop reads also publish partial transfers; no short packet is required
 * to make a command visible to the parser.
 */
#define PCD (&hpcd_USB_OTG_HS)
#define RX_SIZE 8192u
#define TX_SIZE 2048u
#define RX_TRANSFER_BYTES 512u
#define BULK_OUT 0x01u
#define BULK_IN 0x81u
#define NOTIFY_IN 0x82u
#ifndef USB_RAM
/* PCD DMA is disabled: the CPU moves FIFO bytes. DTCM is both reachable by
 * those CPU accesses and faster than the uncached AXI DMA region. */
#define USB_RAM __attribute__((aligned(4)))
#endif
static uint8_t rx[RX_SIZE] USB_RAM, tx[TX_SIZE] USB_RAM;
static uint8_t out_packet[RX_TRANSFER_BYTES] USB_RAM, in_packet[64] USB_RAM;
static uint32_t rx_published;
static volatile uint32_t rx_wr, rx_rd, tx_wr, tx_rd, epoch;
static volatile uint8_t configuration, dtr, suspended, faulted, rx_armed, tx_busy;
static uint8_t tx_zlp;
static volatile uint32_t resets, setups, control_stalls;
static uint8_t last_setup[8];
static uint8_t ctrl[64] __attribute__((aligned(4)));
static uint8_t line_coding[7] = {0, 0xC2, 1, 0, 0, 0, 8};
static const uint8_t *ctrl_next;
static uint16_t ctrl_left;
static uint8_t ctrl_zlp, ctrl_out;
static uint8_t ctrl_stage; /* 0 idle, 1 data IN, 2 data OUT, 3 status IN, 4 status OUT */

/* ---- protect usb queues ------------------------------------------------- */

/* Masking only USB permits a lower-priority mixer interrupt to preempt the
 * main loop with USB still masked for the entire audio refill. Prevent that
 * priority inversion: these sections publish at most one 512-byte receive
 * transfer, copy at most 64 bytes to the parser (or one small reply), and update
 * queue indices/endpoints. Restore the caller's mask. */
static uint32_t lock_usb_queues(void)
{
#if defined(__arm__) || defined(__thumb__)
    uint32_t saved = __get_PRIMASK();
    __disable_irq();
    return saved;
#else
    HAL_NVIC_DisableIRQ(OTG_HS_IRQn);
    return 0;
#endif
}

static void unlock_usb_queues(uint32_t saved)
{
#if defined(__arm__) || defined(__thumb__)
    __set_PRIMASK(saved);
#else
    (void)saved;
    HAL_NVIC_EnableIRQ(OTG_HS_IRQn);
#endif
}

/* ---- handle control endpoint transfers ---------------------------------- */

static void stall_usb_control_endpoint(void)
{
    ++control_stalls;
    ctrl_stage = 0;
    (void)HAL_PCD_EP_SetStall(PCD, 0);
    (void)HAL_PCD_EP_SetStall(PCD, 0x80);
}

static void send_usb_control_status(void)
{
    ctrl_stage = 3;
    (void)HAL_PCD_EP_Transmit(PCD, 0x80, ctrl, 0);
}

static void send_next_usb_control_packet(void)
{
    uint16_t n = ctrl_left > 64 ? 64 : ctrl_left;
    if (n && ctrl_next != ctrl) memcpy(ctrl, ctrl_next, n);
    ctrl_next += n;
    ctrl_left -= n;
    (void)HAL_PCD_EP_Transmit(PCD, 0x80, ctrl, n);
}

static void begin_usb_control_transfer(const uint8_t *data, uint16_t size, uint16_t wanted)
{
    uint16_t n = size < wanted ? size : wanted;
    ctrl_stage = 1;
    ctrl_next = data;
    ctrl_left = n;
    ctrl_zlp = (uint8_t)(n && n < wanted && (n % 64) == 0);
    send_next_usb_control_packet();
}

/* ---- receive bulk endpoint data ----------------------------------------- */

static void arm_usb_receive(void)
{
    /* Reserve room for the entire transfer, including its unpublished tail. */
    if (configuration && !faulted && !rx_armed && RX_SIZE - (rx_wr - rx_rd) >= RX_TRANSFER_BYTES) {
        rx_published = 0;
        rx_armed = 1;
        if (HAL_PCD_EP_Receive(PCD, BULK_OUT, out_packet, RX_TRANSFER_BYTES) != HAL_OK)
            rx_armed = 0;
    }
}

/* Caller holds the queue lock or runs in the USB ISR. With PCD DMA disabled,
 * HAL advances xfer_count only after copying a complete packet from the FIFO.
 * Publishing that prefix early avoids waiting for all eight packets or a ZLP.
 * Completion publishes only the remaining suffix, never the same bytes twice. */
static void publish_usb_received_bytes(uint32_t received)
{
    if (received < rx_published || received > RX_TRANSFER_BYTES ||
        received - rx_published > RX_SIZE - (rx_wr - rx_rd)) {
        faulted = 1;
        ++epoch;
        return;
    }
    uint32_t n = received - rx_published;
    if (!n) return;
    uint32_t wr = rx_wr, at = wr % RX_SIZE, first = n;
    if (first > RX_SIZE - at) first = RX_SIZE - at;
    memcpy(rx + at, out_packet + rx_published, first);
    memcpy(rx, out_packet + rx_published + first, n - first);
    __DMB();
    rx_wr = wr + n;
    rx_published = received;
}

/* ---- transmit queued bulk data ------------------------------------------ */

static void transmit_queued_usb_bytes(void)
{
    if (!configuration || suspended || tx_busy) return;
    uint32_t n = tx_wr - tx_rd;
    if (n > 64) n = 64;
    if (!n && !tx_zlp) return;
    uint32_t at = tx_rd % TX_SIZE, first = n;
    if (first > TX_SIZE - at) first = TX_SIZE - at;
    memcpy(in_packet, tx + at, first);
    memcpy(in_packet + first, tx, n - first);
    tx_busy = 1;
    if (HAL_PCD_EP_Transmit(PCD, BULK_IN, in_packet, n) == HAL_OK) {
        tx_rd += n;
        tx_zlp = (uint8_t)(n == 64);
    } else
        tx_busy = 0;
}

/* ---- reset transport queues and link state ------------------------------ */

static void reset_usb_queues(void)
{
    rx_wr = rx_rd = tx_wr = tx_rd = 0;
    rx_published = 0;
    rx_armed = tx_busy = tx_zlp = 0;
    faulted = 0;
    ++epoch;
}

static void reset_usb_connection(void)
{
    /* DTR is not a USB endpoint reset. Preserve DATA0/DATA1 synchronisation with
     * the host, keep the OUT transaction armed, and discard only application data. */
    uint8_t armed = rx_armed;
    uint32_t received = armed ? HAL_PCD_EP_GetRxCount(PCD, BULK_OUT) : 0;
    if (tx_busy) (void)HAL_PCD_EP_Abort(PCD, BULK_IN);
    (void)HAL_PCD_EP_Flush(PCD, BULK_IN);
#if defined(__arm__) || defined(__thumb__)
    uint32_t USBx_BASE = (uint32_t)PCD->Instance;
    USBx_DEVICE->DIEPEMPMSK &= ~(1u << 1);
    /* Consume the abort completion here; a delayed EPDISD ISR must not flush a
     * newly queued HELLO reply. No DATA PID bits are changed. */
    USBx_INEP(1)->DIEPINT = 0xFFu;
#endif
    reset_usb_queues();
    rx_armed = armed;
    rx_published = received;
    arm_usb_receive();
}

static void close_usb_data_endpoints(void)
{
    (void)HAL_PCD_EP_Close(PCD, BULK_OUT);
    (void)HAL_PCD_EP_Close(PCD, BULK_IN);
    (void)HAL_PCD_EP_Close(PCD, NOTIFY_IN);
}

static void open_usb_data_endpoints(void)
{
    (void)HAL_PCD_EP_Open(PCD, BULK_OUT, 64, EP_TYPE_BULK);
    (void)HAL_PCD_EP_Open(PCD, BULK_IN, 64, EP_TYPE_BULK);
    (void)HAL_PCD_EP_Open(PCD, NOTIFY_IN, 8, EP_TYPE_INTR);
    arm_usb_receive();
}

/* ---- handle usb reset and control requests ------------------------------ */

void HAL_PCD_ResetCallback(PCD_HandleTypeDef *pcd)
{
    (void)pcd;
    ++resets;
    configuration = dtr = suspended = ctrl_stage = 0;
    reset_usb_queues();
    (void)HAL_PCD_EP_Open(PCD, 0, 64, EP_TYPE_CTRL);
    (void)HAL_PCD_EP_Open(PCD, 0x80, 64, EP_TYPE_CTRL);
}

void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *pcd)
{
    const uint8_t *s = (const uint8_t *)pcd->Setup;
    ++setups;
    memcpy(last_setup, s, 8);
    uint8_t bm = s[0], request = s[1];
    uint16_t value = (uint16_t)(s[2] | s[3] << 8), index = (uint16_t)(s[4] | s[5] << 8);
    uint16_t length = (uint16_t)(s[6] | s[7] << 8);
    ctrl_stage = ctrl_out = 0;
    if (bm == 0x80 && request == 6) {
        uint16_t size = 0;
        const uint8_t *desc = get_usb_descriptor((uint8_t)(value >> 8), (uint8_t)value, &size);
        if (desc) {
            begin_usb_control_transfer(desc, size, length);
            return;
        }
    } else if (bm == 0 && request == 5 && value < 128 && !index && !length && !configuration) {
        /* DWC2 latches the address for the status handshake: program it before
         * submitting the ZLP, matching the controller's working device driver. */
        (void)HAL_PCD_SetAddress(PCD, (uint8_t)value);
        send_usb_control_status();
        return;
    } else if (bm == 0 && request == 9 && value <= 1 && !index && !length) {
        if (configuration) close_usb_data_endpoints();
        configuration = (uint8_t)value;
        dtr = 0;
        reset_usb_queues();
        if (configuration) open_usb_data_endpoints();
        send_usb_control_status();
        return;
    } else if (bm == 0x80 && request == 8 && !value && !index && length == 1) {
        ctrl[0] = configuration;
        begin_usb_control_transfer(ctrl, 1, length);
        return;
    } else if ((bm == 0x80 || bm == 0x81 || bm == 0x82) && request == 0 && !value && length == 2) {
        ctrl[0] = ctrl[1] = 0;
        if (bm == 0x80 && index) {
            stall_usb_control_endpoint();
            return;
        }
        if (bm == 0x81 && (!configuration || index > 1)) {
            stall_usb_control_endpoint();
            return;
        }
        if (bm == 0x82) {
            if (index != 0 && index != 0x80 &&
                (!configuration || (index != BULK_OUT && index != BULK_IN && index != NOTIFY_IN))) {
                stall_usb_control_endpoint();
                return;
            }
            ctrl[0] =
                (index & 0x80) ? PCD->IN_ep[index & 15].is_stall : PCD->OUT_ep[index & 15].is_stall;
        }
        begin_usb_control_transfer(ctrl, 2, length);
        return;
    } else if (bm == 2 && (request == 1 || request == 3) && !value && !length && configuration &&
               (index == BULK_OUT || index == BULK_IN || index == NOTIFY_IN)) {
        if (request == 3)
            (void)HAL_PCD_EP_SetStall(PCD, (uint8_t)index);
        else {
            (void)HAL_PCD_EP_ClrStall(PCD, (uint8_t)index);
            if (index == BULK_OUT) {
                rx_armed = 0;
                arm_usb_receive();
            }
            if (index == BULK_IN) {
                tx_busy = 0;
                transmit_queued_usb_bytes();
            }
        }
        send_usb_control_status();
        return;
    } else if (bm == 0x81 && request == 10 && !value && index < 2 && length == 1 && configuration) {
        ctrl[0] = 0;
        begin_usb_control_transfer(ctrl, 1, length);
        return;
    } else if (bm == 1 && request == 11 && !value && index < 2 && !length && configuration) {
        send_usb_control_status();
        return;
    } else if (bm == 0xA1 && request == 0x21 && !value && !index && length == 7 && configuration) {
        begin_usb_control_transfer(line_coding, 7, length);
        return;
    } else if (bm == 0x21 && request == 0x20 && !value && !index && length == 7 && configuration) {
        ctrl_out = 1;
        ctrl_stage = 2;
        (void)HAL_PCD_EP_Receive(PCD, 0, ctrl, 7);
        return;
    } else if (bm == 0x21 && request == 0x22 && !index && !length && configuration && value <= 3) {
        uint8_t next = (uint8_t)(value & 1);
        if (next != dtr) {
            dtr = next;
            reset_usb_connection();
        }
        send_usb_control_status();
        return;
    }
    stall_usb_control_endpoint();
}

/* ---- handle endpoint completions ---------------------------------------- */

void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *pcd, uint8_t ep)
{
    (void)pcd;
    if (ep == 1) {
        tx_busy = 0;
        transmit_queued_usb_bytes();
        return;
    }
    if (ep != 0) return;
    if (ctrl_stage == 1) {
        if (ctrl_left) {
            send_next_usb_control_packet();
            return;
        }
        if (ctrl_zlp) {
            ctrl_zlp = 0;
            (void)HAL_PCD_EP_Transmit(PCD, 0x80, ctrl, 0);
            return;
        }
        ctrl_stage = 4;
        (void)HAL_PCD_EP_Receive(PCD, 0, ctrl, 0);
    } else if (ctrl_stage == 3) {
        ctrl_stage = 0;
    }
}

void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *pcd, uint8_t ep)
{
    uint32_t n = HAL_PCD_EP_GetRxCount(pcd, ep);
    if (ep == 0) {
        if (ctrl_stage == 2 && ctrl_out && n == 7) {
            memcpy(line_coding, ctrl, 7);
            ctrl_out = 0;
            send_usb_control_status();
        } else if (ctrl_stage == 4)
            ctrl_stage = 0;
        else
            stall_usb_control_endpoint();
        return;
    }
    if (ep != 1) return;
    rx_armed = 0;
    if (!dtr || faulted) {
        arm_usb_receive();
        return;
    }
    publish_usb_received_bytes(n);
    arm_usb_receive();
}

/* ---- handle usb link transitions ---------------------------------------- */

void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *pcd)
{
    (void)pcd;
    suspended = 1;
}

void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *pcd)
{
    (void)pcd;
    suspended = 0;
    transmit_queued_usb_bytes();
    arm_usb_receive();
}

void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *pcd)
{
    (void)pcd;
    configuration = dtr = 0;
    reset_usb_queues();
}

/* ---- initialize the usb peripheral -------------------------------------- */

void init_usb_device(void)
{
    /* Preserve the previously working PLL3 USB clock. Generated PCD init has
     * already configured the embedded PHY and the interrupt. No USB DMA. */
    RCC_PeriphCLKInitTypeDef clock = {};
    clock.PeriphClockSelection = RCC_PERIPHCLK_USB;
    clock.PLL3.PLL3M = 4;
    clock.PLL3.PLL3N = 125;
    clock.PLL3.PLL3P = 16;
    clock.PLL3.PLL3Q = 16;
    clock.PLL3.PLL3R = 8;
    clock.PLL3.PLL3RGE = RCC_PLL3VCIRANGE_2;
    clock.PLL3.PLL3VCOSEL = RCC_PLL3VCOWIDE;
    clock.UsbClockSelection = RCC_USBCLKSOURCE_PLL3;
    if (HAL_RCCEx_PeriphCLKConfig(&clock) != HAL_OK) Error_Handler();
    HAL_NVIC_DisableIRQ(OTG_HS_IRQn);
    /* FIFO sizes in 32-bit words; total 256 words fits the core's FIFO RAM. */
    if (HAL_PCDEx_SetRxFiFo(PCD, 128) != HAL_OK || HAL_PCDEx_SetTxFiFo(PCD, 0, 32) != HAL_OK ||
        HAL_PCDEx_SetTxFiFo(PCD, 1, 64) != HAL_OK || HAL_PCDEx_SetTxFiFo(PCD, 2, 32) != HAL_OK ||
        HAL_PCD_Start(PCD) != HAL_OK)
        Error_Handler();
    HAL_NVIC_EnableIRQ(OTG_HS_IRQn);
}

/* ---- read and write transport queues ------------------------------------ */

uint32_t read_usb_bytes(uint8_t *dst, uint32_t size)
{
    uint32_t saved = lock_usb_queues();
    if (dtr && !faulted && rx_armed)
        publish_usb_received_bytes(HAL_PCD_EP_GetRxCount(PCD, BULK_OUT));
    if (faulted) {
        unlock_usb_queues(saved);
        return 0;
    }
    uint32_t n = rx_wr - rx_rd;
    if (n > size) n = size;
    if (n > 64) n = 64; /* Keep the interrupt-masked copy bounded. */
    uint32_t rd = rx_rd, at = rd % RX_SIZE, first = n;
    if (first > RX_SIZE - at) first = RX_SIZE - at;
    memcpy(dst, rx + at, first);
    memcpy(dst + first, rx, n - first);
    rx_rd = rd + n;
    arm_usb_receive();
    unlock_usb_queues(saved);
    return n;
}

uint8_t write_usb_bytes(const uint8_t *src, uint32_t size)
{
    uint32_t saved = lock_usb_queues();
    uint8_t ok = (uint8_t)(configuration && dtr && size <= TX_SIZE - (tx_wr - tx_rd));
    if (ok) {
        uint32_t wr = tx_wr, at = wr % TX_SIZE, first = size;
        if (first > TX_SIZE - at) first = TX_SIZE - at;
        memcpy(tx + at, src, first);
        memcpy(tx, src + first, size - first);
        tx_wr = wr + size;
        transmit_queued_usb_bytes();
    }
    unlock_usb_queues(saved);
    return ok;
}

/* ---- query transport state ---------------------------------------------- */

uint32_t get_usb_transmit_free_bytes(void)
{
    return TX_SIZE - (tx_wr - tx_rd);
}

uint32_t get_usb_connection_generation(void)
{
    return epoch;
}

uint8_t is_usb_connected(void)
{
    return (uint8_t)(configuration && dtr && !faulted && !suspended);
}

void fault_usb_connection(void)
{
    uint32_t saved = lock_usb_queues();
    /* Let any already armed packet finish and then leave OUT NAKed. STALL/clear
     * would reset endpoint PID on only one side during DTR recovery. */
    faulted = 1;
    unlock_usb_queues(saved);
}

void format_usb_device_status(char *text, uint32_t size)
{
    uint32_t gint = 0, mask = 0, ahb = 0, cfg = 0, ctl = 0, otg = 0, phy = 0;
#if defined(__arm__) || defined(__thumb__)
    uint32_t USBx_BASE = (uint32_t)PCD->Instance;
    gint = PCD->Instance->GINTSTS;
    mask = PCD->Instance->GINTMSK;
    ahb = PCD->Instance->GAHBCFG;
    cfg = USBx_DEVICE->DCFG;
    ctl = USBx_DEVICE->DCTL;
    otg = PCD->Instance->GOTGCTL;
    phy = PCD->Instance->GCCFG;
#endif
    (void)snprintf(
        text, size,
        "ok: usbdev reset %lu setup %lu stall %lu state %u cfg %u dtr %u suspend %u fault %u "
        "last %02x%02x%02x%02x%02x%02x%02x%02x "
        "regs %08lx %08lx %08lx %08lx %08lx %08lx %08lx\r\n",
        (unsigned long)resets, (unsigned long)setups, (unsigned long)control_stalls, ctrl_stage,
        configuration, dtr, suspended, faulted, last_setup[0], last_setup[1], last_setup[2],
        last_setup[3], last_setup[4], last_setup[5], last_setup[6], last_setup[7],
        (unsigned long)gint, (unsigned long)mask, (unsigned long)ahb, (unsigned long)cfg,
        (unsigned long)ctl, (unsigned long)otg, (unsigned long)phy);
}

/* ---- describe the usb device -------------------------------------------- */

/* Single CDC ACM function; no audio interfaces. */
static const uint8_t device[] = {18,
                                 1,
                                 0,
                                 2,
                                 0xEF,
                                 2,
                                 1,
                                 64,
                                 USB_STREAM_VID & 255,
                                 USB_STREAM_VID >> 8,
                                 USB_STREAM_PID & 255,
                                 USB_STREAM_PID >> 8,
                                 0,
                                 3,
                                 1,
                                 2,
                                 3,
                                 1};
static const uint8_t configuration_descriptor[] = {
    9,    2, 75, 0,    2, 1, 0,    0x80, 50,   8,    11,   0, 2, 2,    2, 1,  0, 9, 4,
    0,    0, 1,  2,    2, 1, 0,    5,    0x24, 0,    0x10, 1, 5, 0x24, 1, 0,  1, 4, 0x24,
    2,    2, 5,  0x24, 6, 0, 1,    7,    5,    0x82, 3,    8, 0, 16,   9, 4,  1, 0, 2,
    0x0A, 0, 0,  0,    7, 5, 0x01, 2,    64,   0,    0,    7, 5, 0x81, 2, 64, 0, 0};
static_assert(sizeof(configuration_descriptor) == 75, "CDC descriptor length");

/* ---- select a usb descriptor -------------------------------------------- */

const uint8_t *get_usb_descriptor(uint8_t type, uint8_t index, uint16_t *size)
{
    static uint8_t string[66];
    char serial[32];
    const char *text;
    if (type == 1 && index == 0) {
        *size = sizeof(device);
        return device;
    }
    if (type == 2 && index == 0) {
        *size = sizeof(configuration_descriptor);
        return configuration_descriptor;
    }
    if (type != 3 || index > 3) return NULL;
    if (index == 0) {
        string[0] = 4;
        string[1] = 3;
        string[2] = 9;
        string[3] = 4;
        *size = 4;
        return string;
    }
    (void)snprintf(serial, sizeof serial, "CHCARD-%08lX%08lX%08lX", (unsigned long)HAL_GetUIDw0(),
                   (unsigned long)HAL_GetUIDw1(), (unsigned long)HAL_GetUIDw2());
    text = index == 1 ? "Freshwater" : index == 2 ? "Channel Card Data" : serial;
    *size = (uint16_t)(2 + 2 * strlen(text));
    string[0] = (uint8_t)*size;
    string[1] = 3;
    for (unsigned i = 0; text[i]; ++i) {
        string[2 + 2 * i] = (uint8_t)text[i];
        string[3 + 2 * i] = 0;
    }
    return string;
}
