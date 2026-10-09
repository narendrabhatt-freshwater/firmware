#include "usb.h"
#include "voice.h"
#include "channel.h"
#include "samples.h"
#include "stream.h"
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
static uint32_t queue_lock(void)
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

static void queue_unlock(uint32_t saved)
{
#if defined(__arm__) || defined(__thumb__)
    __set_PRIMASK(saved);
#else
    (void)saved;
    HAL_NVIC_EnableIRQ(OTG_HS_IRQn);
#endif
}

/* ---- handle control endpoint transfers ---------------------------------- */

static void stall_control(void)
{
    ++control_stalls;
    ctrl_stage = 0;
    (void)HAL_PCD_EP_SetStall(PCD, 0);
    (void)HAL_PCD_EP_SetStall(PCD, 0x80);
}

static void status_in(void)
{
    ctrl_stage = 3;
    (void)HAL_PCD_EP_Transmit(PCD, 0x80, ctrl, 0);
}

static void next_control_in(void)
{
    uint16_t n = ctrl_left > 64 ? 64 : ctrl_left;
    if (n && ctrl_next != ctrl) memcpy(ctrl, ctrl_next, n);
    ctrl_next += n;
    ctrl_left -= n;
    (void)HAL_PCD_EP_Transmit(PCD, 0x80, ctrl, n);
}

static void control_in(const uint8_t *data, uint16_t size, uint16_t wanted)
{
    uint16_t n = size < wanted ? size : wanted;
    ctrl_stage = 1;
    ctrl_next = data;
    ctrl_left = n;
    ctrl_zlp = (uint8_t)(n && n < wanted && (n % 64) == 0);
    next_control_in();
}

/* ---- receive bulk endpoint data ----------------------------------------- */

static void arm_out(void)
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
static void publish_out(uint32_t received)
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

static void kick_in(void)
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

static void reset_queues(void)
{
    rx_wr = rx_rd = tx_wr = tx_rd = 0;
    rx_published = 0;
    rx_armed = tx_busy = tx_zlp = 0;
    faulted = 0;
    ++epoch;
}

static void reset_link(void)
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
    reset_queues();
    rx_armed = armed;
    rx_published = received;
    arm_out();
}

static void close_data(void)
{
    (void)HAL_PCD_EP_Close(PCD, BULK_OUT);
    (void)HAL_PCD_EP_Close(PCD, BULK_IN);
    (void)HAL_PCD_EP_Close(PCD, NOTIFY_IN);
}

static void open_data(void)
{
    (void)HAL_PCD_EP_Open(PCD, BULK_OUT, 64, EP_TYPE_BULK);
    (void)HAL_PCD_EP_Open(PCD, BULK_IN, 64, EP_TYPE_BULK);
    (void)HAL_PCD_EP_Open(PCD, NOTIFY_IN, 8, EP_TYPE_INTR);
    arm_out();
}

/* ---- handle usb reset and control requests ------------------------------ */

void HAL_PCD_ResetCallback(PCD_HandleTypeDef *pcd)
{
    (void)pcd;
    ++resets;
    configuration = dtr = suspended = ctrl_stage = 0;
    reset_queues();
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
        const uint8_t *desc = USB_Descriptor((uint8_t)(value >> 8), (uint8_t)value, &size);
        if (desc) {
            control_in(desc, size, length);
            return;
        }
    } else if (bm == 0 && request == 5 && value < 128 && !index && !length && !configuration) {
        /* DWC2 latches the address for the status handshake: program it before
         * submitting the ZLP, matching the controller's working device driver. */
        (void)HAL_PCD_SetAddress(PCD, (uint8_t)value);
        status_in();
        return;
    } else if (bm == 0 && request == 9 && value <= 1 && !index && !length) {
        if (configuration) close_data();
        configuration = (uint8_t)value;
        dtr = 0;
        reset_queues();
        if (configuration) open_data();
        status_in();
        return;
    } else if (bm == 0x80 && request == 8 && !value && !index && length == 1) {
        ctrl[0] = configuration;
        control_in(ctrl, 1, length);
        return;
    } else if ((bm == 0x80 || bm == 0x81 || bm == 0x82) && request == 0 && !value && length == 2) {
        ctrl[0] = ctrl[1] = 0;
        if (bm == 0x80 && index) {
            stall_control();
            return;
        }
        if (bm == 0x81 && (!configuration || index > 1)) {
            stall_control();
            return;
        }
        if (bm == 0x82) {
            if (index != 0 && index != 0x80 &&
                (!configuration || (index != BULK_OUT && index != BULK_IN && index != NOTIFY_IN))) {
                stall_control();
                return;
            }
            ctrl[0] =
                (index & 0x80) ? PCD->IN_ep[index & 15].is_stall : PCD->OUT_ep[index & 15].is_stall;
        }
        control_in(ctrl, 2, length);
        return;
    } else if (bm == 2 && (request == 1 || request == 3) && !value && !length && configuration &&
               (index == BULK_OUT || index == BULK_IN || index == NOTIFY_IN)) {
        if (request == 3)
            (void)HAL_PCD_EP_SetStall(PCD, (uint8_t)index);
        else {
            (void)HAL_PCD_EP_ClrStall(PCD, (uint8_t)index);
            if (index == BULK_OUT) {
                rx_armed = 0;
                arm_out();
            }
            if (index == BULK_IN) {
                tx_busy = 0;
                kick_in();
            }
        }
        status_in();
        return;
    } else if (bm == 0x81 && request == 10 && !value && index < 2 && length == 1 && configuration) {
        ctrl[0] = 0;
        control_in(ctrl, 1, length);
        return;
    } else if (bm == 1 && request == 11 && !value && index < 2 && !length && configuration) {
        status_in();
        return;
    } else if (bm == 0xA1 && request == 0x21 && !value && !index && length == 7 && configuration) {
        control_in(line_coding, 7, length);
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
            reset_link();
        }
        status_in();
        return;
    }
    stall_control();
}

/* ---- handle endpoint completions ---------------------------------------- */

void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *pcd, uint8_t ep)
{
    (void)pcd;
    if (ep == 1) {
        tx_busy = 0;
        kick_in();
        return;
    }
    if (ep != 0) return;
    if (ctrl_stage == 1) {
        if (ctrl_left) {
            next_control_in();
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
            status_in();
        } else if (ctrl_stage == 4)
            ctrl_stage = 0;
        else
            stall_control();
        return;
    }
    if (ep != 1) return;
    rx_armed = 0;
    if (!dtr || faulted) {
        arm_out();
        return;
    }
    publish_out(n);
    arm_out();
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
    kick_in();
    arm_out();
}

void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *pcd)
{
    (void)pcd;
    configuration = dtr = 0;
    reset_queues();
}

/* ---- initialize the usb peripheral -------------------------------------- */

void USB_Device_Init(void)
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

uint32_t USB_Device_Read(uint8_t *dst, uint32_t size)
{
    uint32_t saved = queue_lock();
    if (dtr && !faulted && rx_armed) publish_out(HAL_PCD_EP_GetRxCount(PCD, BULK_OUT));
    if (faulted) {
        queue_unlock(saved);
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
    arm_out();
    queue_unlock(saved);
    return n;
}

uint8_t USB_Device_Write(const uint8_t *src, uint32_t size)
{
    uint32_t saved = queue_lock();
    uint8_t ok = (uint8_t)(configuration && dtr && size <= TX_SIZE - (tx_wr - tx_rd));
    if (ok) {
        uint32_t wr = tx_wr, at = wr % TX_SIZE, first = size;
        if (first > TX_SIZE - at) first = TX_SIZE - at;
        memcpy(tx + at, src, first);
        memcpy(tx, src + first, size - first);
        tx_wr = wr + size;
        kick_in();
    }
    queue_unlock(saved);
    return ok;
}

/* ---- query transport state ---------------------------------------------- */

uint32_t USB_Device_TxFree(void)
{
    return TX_SIZE - (tx_wr - tx_rd);
}

uint32_t USB_Device_Epoch(void)
{
    return epoch;
}

uint8_t USB_Device_Connected(void)
{
    return (uint8_t)(configuration && dtr && !faulted && !suspended);
}

void USB_Device_Fault(void)
{
    uint32_t saved = queue_lock();
    /* Let any already armed packet finish and then leave OUT NAKed. STALL/clear
     * would reset endpoint PID on only one side during DTR recovery. */
    faulted = 1;
    queue_unlock(saved);
}

void USB_Device_Debug(char *text, uint32_t size)
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

const uint8_t *USB_Descriptor(uint8_t type, uint8_t index, uint16_t *size)
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

/* ---- receive attack and wavetable uploads ------------------------------- */

/* CDC session: al → 1..ATTACK_BANK_BYTES signed int8 → Commit. */

static uint8_t s_active;
static uint16_t s_id;
static uint32_t s_need;
static uint32_t s_got;
static int8_t *s_dst;
static uint8_t s_wavetable_upload;
static uint8_t s_logical_wave;

/* ---- query and abort an attack upload ----------------------------------- */

uint8_t AttackUpload_IsActive(void)
{
    return s_active;
}

void AttackUpload_Abort(void)
{
    AttackBank_SetWriteActive(0u);
    s_active = 0u;
    s_dst = NULL;
    s_need = 0u;
    s_got = 0u;
    s_wavetable_upload = 0u;
    s_logical_wave = 0u;
}

/* ---- begin sample and wavetable uploads --------------------------------- */

static int AttackUpload_BeginResolved(uint16_t wave_id, uint32_t nbytes, uint8_t wavetable_upload,
                                      uint8_t logical_wave)
{
    if (wave_id >= ATTACK_BANK_COUNT || nbytes == 0u || nbytes > ATTACK_BANK_BYTES) {
        return -1;
    }
    if (s_active != 0u || (wavetable_upload != 0u && NoteBank_AnyBankReferences() != 0u)) {
        return -1;
    }

    AttackBank_SetWriteActive(wavetable_upload);
    s_dst = AttackBank_WritePtr(wave_id);
    if (s_dst == NULL) {
        AttackBank_SetWriteActive(0u);
        return -1;
    }

    s_id = wave_id;
    s_need = nbytes;
    s_got = 0u;
    s_wavetable_upload = wavetable_upload;
    s_logical_wave = logical_wave;
    s_active = 1u;
    return 0;
}

int AttackUpload_Begin(uint16_t wave_id, uint32_t nbytes)
{
    if (wave_id >= ATTACK_BANK_SAMPLE_COUNT) {
        return -1;
    }
    return AttackUpload_BeginResolved(wave_id, nbytes, 0u, 0u);
}

int AttackUpload_BeginWavetable(uint8_t wave, uint32_t nbytes)
{
    if (wave >= ATTACK_BANK_WAVETABLE_COUNT || nbytes < 2u) {
        return -1;
    }
    return AttackUpload_BeginResolved((uint16_t)(ATTACK_BANK_WAVETABLE_FIRST + wave), nbytes, 1u,
                                      wave);
}

/* ---- receive attack upload data ----------------------------------------- */

uint32_t AttackUpload_Feed(const uint8_t *buf, uint32_t len)
{
    uint32_t take;
    uint32_t i;
    char msg[48];

    if (s_active == 0u || buf == NULL || s_dst == NULL) {
        return 0u;
    }

    take = len;
    if (take > (s_need - s_got)) {
        take = s_need - s_got;
    }

    {
        uint8_t *dst_bytes = (uint8_t *)s_dst;
        for (i = 0u; i < take; i++) {
            dst_bytes[s_got + i] = buf[i];
        }
    }
    s_got += take;

    if (s_got >= s_need) {
        /* Keep the old length during transfer. Only the completed upload updates
         * metadata; the payload itself overwrites live storage as bytes arrive. */
        for (i = s_need; i < ATTACK_BANK_BYTES; i++)
            s_dst[i] = 0;

        if (AttackBank_Commit(s_id, s_need) != 0) {
            USB_CDC_WriteStr("err:range\r\n");
        } else {
            if (s_wavetable_upload != 0u) {
                (void)snprintf(msg, sizeof msg, "ok:wavetable %u\r\n", (unsigned)s_logical_wave);
            } else {
                (void)snprintf(msg, sizeof msg, "ok:attack %u\r\n", (unsigned)s_id);
            }
            USB_CDC_WriteStr(msg);
        }
        AttackUpload_Abort();
    }

    return take;
}

/* ---- dispatch binary usb messages --------------------------------------- */

/* Binary-only CDC: BODY + chunked uploads, with controls/status on RS485. */
static USB_Parser parser;
static uint32_t link_epoch, last_rx_ms, upload_ms;
static uint32_t rx_messages, rx_bytes, blocks, bad;
static uint8_t hello, failed, upload_kind, upload_target;
static uint32_t upload_total, upload_offset;
static uint32_t probe_bytes, probe_blocks, probe_hash;
static char upload_reply[96];
static uint8_t reply[USB_STREAM_HEADER_BYTES + 128];
static uint16_t reply_size;

/* Existing upload validators report here; strings never enter the raw stream. */
void USB_CDC_WriteStr(const char *s)
{
    if (s) (void)snprintf(upload_reply, sizeof upload_reply, "%s", s);
}

/* ---- manage binary uploads and replies ---------------------------------- */

static void abort_upload(void)
{
    VmUpload_Abort();
    AttackUpload_Abort();
    upload_kind = 0;
    upload_offset = upload_total = 0;
}

static void make_reply(const uint8_t *header, const uint8_t *payload, uint16_t size)
{
    memcpy(reply, header, USB_STREAM_HEADER_BYTES);
    reply[0] = USB_MSG_REPLY;
    /* Request type occupies the reply's session byte; no request IDs are needed
     * because exactly one request/reply exchange may be outstanding. */
    reply[2] = header[0];
    USB_Write16(reply + 3, size);
    memcpy(reply + USB_STREAM_HEADER_BYTES, payload, size);
    reply_size = (uint16_t)(USB_STREAM_HEADER_BYTES + size);
}

static void result(const uint8_t *h, uint8_t error, const char *message)
{
    uint8_t data[96];
    data[0] = error;
    size_t n = message ? strlen(message) : 0;
    if (n > sizeof(data) - 1) n = sizeof(data) - 1;
    if (n) memcpy(data + 1, message, n);
    make_reply(h, data, (uint16_t)(n + 1));
}

static void fail_link(const uint8_t *h, const char *message)
{
    ++bad;
    failed = 1;
    hello = 0;
    abort_upload();
    if (h) result(h, 1, message);
    USB_Device_Fault();
}

/* ---- dispatch a binary usb message -------------------------------------- */

static uint8_t dispatch(const uint8_t *h)
{
    uint16_t n = USB_Read16(h + 3);
    const uint8_t *p = h + USB_STREAM_HEADER_BYTES;
    if (h[0] == USB_MSG_HELLO) {
        if (hello || n != 1 || p[0] != USB_STREAM_VERSION || h[1] || h[2]) {
            fail_link(h, "incompatible HELLO");
            return 1;
        }
        abort_upload();
        uint16_t processed = StreamRing_LastBodySequence();
        hello = 1;
        probe_bytes = probe_blocks = 0;
        probe_hash = 2166136261u;
        const uint8_t caps[] = {0,
                                USB_STREAM_VERSION,
                                8,
                                1,
                                0x80,
                                0xBB,
                                0,
                                0,
                                USB_STREAM_PAYLOAD_MAX & 255,
                                USB_STREAM_PAYLOAD_MAX >> 8,
                                USB_STREAM_PRIME_SAMPLES & 255,
                                USB_STREAM_PRIME_SAMPLES >> 8,
                                (uint8_t)(processed & 255),
                                (uint8_t)(processed >> 8)};
        make_reply(h, caps, sizeof caps);
        return 1;
    }
    if (!hello) {
        fail_link(h, "HELLO required");
        return 1;
    }
    /* Diagnostic sink: no voice, upload, or credit state is changed. An empty
     * probe is an ordered barrier returning totals for this connection. */
    if (h[0] == USB_MSG_PROBE) {
        if (h[1] || h[2]) {
            fail_link(h, "invalid PROBE");
            return 1;
        }
        if (n) {
            probe_bytes += n;
            ++probe_blocks;
            for (uint16_t i = 0; i < n; ++i)
                probe_hash = (probe_hash ^ p[i]) * 16777619u;
        } else {
            uint8_t stats[13] = {};
            uint32_t values[] = {probe_bytes, probe_blocks, probe_hash};
            for (unsigned i = 0; i < 3; ++i) {
                USB_Write16(stats + 1 + i * 4, (uint16_t)values[i]);
                USB_Write16(stats + 3 + i * 4, (uint16_t)(values[i] >> 16));
            }
            make_reply(h, stats, sizeof stats);
        }
        return 1;
    }
    if (h[0] == USB_MSG_BODY) {
        if (!n || h[1] >= 8 || h[2] >= USB_STREAM_SESSION_MOD) {
            fail_link(h, "invalid BODY");
            return 1;
        }
        /* Prediction may temporarily run ahead of consumption. Retain this whole
         * block and apply USB backpressure; never overwrite or drop ring samples. */
        if ((StreamRing_TargetSession(h[1]) == h[2] || StreamRing_CurrentSession(h[1]) == h[2]) &&
            StreamRing_FreeLevel(h[1]) < ((n + 15u) / 16u) * 16u)
            return 0;
        /* Stale notes are acknowledged/ignored by the ring; no stale data becomes
         * audible. Capacity errors are explicit, never silently dropped. */
        int accepted = StreamRing_WriteBody(h[1], h[2], (const int8_t *)p, n);
        if (accepted < 0) {
            fail_link(h, "BODY ring capacity");
            return 1;
        }
        rx_messages += (uint32_t)(accepted > 0);
        return 1;
    }
    if (h[2]) {
        fail_link(h, "invalid upload header");
        return 1;
    }
    upload_reply[0] = 0;
    if (h[0] == USB_MSG_UPLOAD_BEGIN) {
        if (n != 5 || upload_kind) {
            result(h, 1, "upload busy/invalid");
            return 1;
        }
        uint32_t size = USB_Read32(p + 1);
        int rc = -1;
        if (p[0] == USB_UPLOAD_ATTACK)
            rc = AttackUpload_Begin(h[1], size);
        else if (p[0] == USB_UPLOAD_WAVE)
            rc = AttackUpload_BeginWavetable(h[1], size);
        else if (p[0] == USB_UPLOAD_SCRIPT)
            rc = VmUpload_Begin(h[1], size);
        if (rc) {
            result(h, 1, "upload rejected (target, size, or active voice)");
            return 1;
        }
        upload_kind = p[0];
        upload_target = h[1];
        upload_total = size;
        upload_offset = 0;
        upload_ms = HAL_GetTick();
        result(h, 0, NULL);
        return 1;
    }
    if (h[0] == USB_MSG_UPLOAD_DATA) {
        if (!upload_kind || h[1] != upload_target || n <= 4 || USB_Read32(p) != upload_offset ||
            (uint32_t)(n - 4) > upload_total - upload_offset) {
            abort_upload();
            result(h, 1, "upload offset/length");
            return 1;
        }
        uint32_t take = upload_kind == USB_UPLOAD_SCRIPT ? VmUpload_Feed(p + 4, n - 4)
                                                         : AttackUpload_Feed(p + 4, n - 4);
        if (take != (uint32_t)(n - 4) || strncmp(upload_reply, "err:", 4) == 0) {
            result(h, 1, upload_reply[0] ? upload_reply : "upload failed");
            abort_upload();
            return 1;
        }
        upload_offset += take;
        upload_ms = HAL_GetTick();
        result(h, 0, NULL);
        if (upload_offset == upload_total) abort_upload();
        return 1;
    }
    if (h[0] == USB_MSG_UPLOAD_ABORT && n == 0) {
        abort_upload();
        result(h, 0, NULL);
        return 1;
    }
    fail_link(h, "unknown request");
    return 1;
}

/* ---- initialize the binary usb transport -------------------------------- */

void USB_App_Init(void)
{
    USB_ParserReset(&parser);
    USB_Device_Init();
    link_epoch = USB_Device_Epoch();
}

/* ---- receive and process usb messages ----------------------------------- */

void USB_App_Task(void)
{
    uint32_t epoch = USB_Device_Epoch();
    if (epoch != link_epoch) {
        abort_upload();
        USB_ParserReset(&parser);
        hello = failed = 0;
        reply_size = 0;
        link_epoch = epoch;
    }
    if (reply_size) {
        if (!USB_Device_Write(reply, reply_size)) return;
        reply_size = 0;
    }
    if (!USB_Device_Connected() || failed) return;
    if (parser.used == parser.need) {
        if (!dispatch(parser.bytes)) return;
        USB_ParserReset(&parser);
        if (failed || reply_size) return;
    }
    /* Bounded passes keep the main-loop RS485 service responsive. */
    uint32_t budget = 4096;
    while (budget && !reply_size && !failed) {
        /* Never consume beyond this frame: a pending reply must not discard the
         * beginning of the next block from the same USB packet. */
        uint32_t want = parser.need - parser.used;
        if (want > budget) want = budget;
        uint32_t n = USB_Device_Read(parser.bytes + parser.used, want);
        if (USB_Device_Epoch() != link_epoch) return;
        if (!n) break;
        rx_bytes += n;
        budget -= n;
        last_rx_ms = HAL_GetTick();
        int rc = USB_ParserCommit(&parser, (uint16_t)n);
        if (rc < 0) {
            fail_link(NULL, "bad header");
            return;
        }
        if (rc > 0) {
            ++blocks;
            if (dispatch(parser.bytes)) USB_ParserReset(&parser);
        }
    }
    if (parser.used && parser.used < parser.need && (uint32_t)(HAL_GetTick() - last_rx_ms) > 1000)
        fail_link(NULL, "partial frame timeout");
    if (upload_kind && (uint32_t)(HAL_GetTick() - upload_ms) > 5000) abort_upload();
}

/* ---- query and clear transport statistics ------------------------------- */

uint16_t USB_App_LastPackSequence(void)
{
    return StreamRing_LastBodySequence();
}

uint32_t USB_App_RxMsgCount(void)
{
    return rx_messages;
}

uint32_t USB_App_RxByteCount(void)
{
    return rx_bytes;
}

uint32_t USB_App_BlockCount(void)
{
    return blocks;
}

uint32_t USB_App_BadCount(void)
{
    return bad;
}

uint32_t USB_App_BadReasonCount(uint8_t reason)
{
    return reason == 4 ? bad : 0;
}

void USB_App_StatsClear(void)
{
    rx_messages = rx_bytes = blocks = bad = 0;
}
