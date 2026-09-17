/* Minimal full-speed CDC ACM device on the existing STM32 HAL PCD.
 * IRQ owns endpoints. Queue publication uses short interrupt critical sections.
 * OUT is rearmed in its completion ISR, not on the 1 ms application cadence.
 */
#include "usb_device.h"
#include "usb_otg.h"
#include "main.h"
#include <string.h>
#include <stdio.h>
#define PCD (&hpcd_USB_OTG_HS)
#define RX_SIZE 8192u
#define TX_SIZE 2048u
#define BULK_OUT 0x01u
#define BULK_IN 0x81u
#define NOTIFY_IN 0x82u
#ifndef USB_RAM
/* PCD DMA is disabled: the CPU moves FIFO bytes. DTCM is both reachable by
 * those CPU accesses and faster than the uncached AXI DMA region. */
#define USB_RAM __attribute__((aligned(4)))
#endif
static uint8_t rx[RX_SIZE] USB_RAM, tx[TX_SIZE] USB_RAM;
static uint8_t out_packet[64] USB_RAM, in_packet[64] USB_RAM;
static volatile uint32_t rx_wr, rx_rd, tx_wr, tx_rd, epoch;
static volatile uint8_t configuration, dtr, suspended, faulted, rx_armed, tx_busy;
static uint8_t tx_zlp;
static volatile uint32_t resets, setups, control_stalls;
static uint8_t last_setup[8];
static uint8_t ctrl[64] __attribute__((aligned(4)));
static uint8_t line_coding[7] = {0,0xC2,1,0,0,0,8};
static const uint8_t *ctrl_next;
static uint16_t ctrl_left;
static uint8_t ctrl_zlp, ctrl_out;
static uint8_t ctrl_stage; /* 0 idle, 1 data IN, 2 data OUT, 3 status IN, 4 status OUT */

/* Masking only USB permits a lower-priority mixer interrupt to preempt the
 * main loop with USB still masked for the entire audio refill. Prevent that
 * priority inversion: these sections copy at most one RX packet or one small
 * reply, update queue indices and arm an endpoint. Restore the caller's mask. */
static uint32_t queue_lock(void)
{
#if defined(__arm__) || defined(__thumb__)
  uint32_t saved=__get_PRIMASK();
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

static void stall_control(void)
{
  ++control_stalls;
  ctrl_stage = 0;
  (void)HAL_PCD_EP_SetStall(PCD, 0);
  (void)HAL_PCD_EP_SetStall(PCD, 0x80);
}
static void status_in(void)
{ ctrl_stage=3; (void)HAL_PCD_EP_Transmit(PCD, 0x80, ctrl, 0); }
static void next_control_in(void)
{
  uint16_t n = ctrl_left > 64 ? 64 : ctrl_left;
  if (n && ctrl_next!=ctrl) memcpy(ctrl, ctrl_next, n);
  ctrl_next += n; ctrl_left -= n;
  (void)HAL_PCD_EP_Transmit(PCD, 0x80, ctrl, n);
}
static void control_in(const uint8_t *data, uint16_t size, uint16_t wanted)
{
  uint16_t n = size < wanted ? size : wanted;
  ctrl_stage=1; ctrl_next=data; ctrl_left=n;
  ctrl_zlp=(uint8_t)(n && n < wanted && (n % 64) == 0);
  next_control_in();
}
static void arm_out(void)
{
  if (configuration && !faulted && !rx_armed && RX_SIZE-(rx_wr-rx_rd) >= 64) {
    rx_armed=1;
    if (HAL_PCD_EP_Receive(PCD, BULK_OUT, out_packet, 64) != HAL_OK) rx_armed=0;
  }
}
static void kick_in(void)
{
  if (!configuration || suspended || tx_busy) return;
  uint32_t n=tx_wr-tx_rd;
  if (n > 64) n=64;
  if (!n && !tx_zlp) return;
  uint32_t at=tx_rd%TX_SIZE, first=n;
  if(first>TX_SIZE-at)first=TX_SIZE-at;
  memcpy(in_packet,tx+at,first); memcpy(in_packet+first,tx,n-first);
  tx_busy=1;
  if (HAL_PCD_EP_Transmit(PCD, BULK_IN, in_packet, n) == HAL_OK) {
    tx_rd+=n; tx_zlp=(uint8_t)(n==64);
  } else tx_busy=0;
}
static void reset_queues(void)
{
  rx_wr=rx_rd=tx_wr=tx_rd=0;
  rx_armed=tx_busy=tx_zlp=0;
  faulted=0; ++epoch;
}
static void reset_link(void)
{
  /* DTR is not a USB endpoint reset. Preserve DATA0/DATA1 synchronisation with
   * the host, keep the OUT transaction armed, and discard only application data. */
  uint8_t armed=rx_armed;
  if (tx_busy) (void)HAL_PCD_EP_Abort(PCD,BULK_IN);
  (void)HAL_PCD_EP_Flush(PCD,BULK_IN);
#if defined(__arm__) || defined(__thumb__)
  uint32_t USBx_BASE=(uint32_t)PCD->Instance;
  USBx_DEVICE->DIEPEMPMSK &= ~(1u<<1);
  /* Consume the abort completion here; a delayed EPDISD ISR must not flush a
   * newly queued HELLO reply. No DATA PID bits are changed. */
  USBx_INEP(1)->DIEPINT=0xFFu;
#endif
  reset_queues(); rx_armed=armed; arm_out();
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
void HAL_PCD_ResetCallback(PCD_HandleTypeDef *pcd)
{
  (void)pcd;
  ++resets;
  configuration=dtr=suspended=ctrl_stage=0;
  reset_queues();
  (void)HAL_PCD_EP_Open(PCD, 0, 64, EP_TYPE_CTRL);
  (void)HAL_PCD_EP_Open(PCD, 0x80, 64, EP_TYPE_CTRL);
}
void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *pcd)
{
  const uint8_t *s=(const uint8_t *)pcd->Setup;
  ++setups; memcpy(last_setup,s,8);
  uint8_t bm=s[0], request=s[1];
  uint16_t value=(uint16_t)(s[2]|s[3]<<8), index=(uint16_t)(s[4]|s[5]<<8);
  uint16_t length=(uint16_t)(s[6]|s[7]<<8);
  ctrl_stage=ctrl_out=0;
  if (bm==0x80 && request==6) {
    uint16_t size=0;
    const uint8_t *desc=USB_Descriptor((uint8_t)(value>>8),(uint8_t)value,&size);
    if (desc) { control_in(desc,size,length); return; }
  } else if (bm==0 && request==5 && value<128 && !index && !length && !configuration) {
    /* DWC2 latches the address for the status handshake: program it before
     * submitting the ZLP, matching the controller's working device driver. */
    (void)HAL_PCD_SetAddress(PCD,(uint8_t)value); status_in(); return;
  } else if (bm==0 && request==9 && value<=1 && !index && !length) {
    if (configuration) close_data();
    configuration=(uint8_t)value; dtr=0; reset_queues();
    if (configuration) open_data();
    status_in(); return;
  } else if (bm==0x80 && request==8 && !value && !index && length==1) {
    ctrl[0]=configuration; control_in(ctrl,1,length); return;
  } else if ((bm==0x80 || bm==0x81 || bm==0x82) && request==0 && !value && length==2) {
    ctrl[0]=ctrl[1]=0;
    if (bm==0x80 && index) { stall_control(); return; }
    if (bm==0x81 && (!configuration || index>1)) { stall_control(); return; }
    if (bm==0x82) {
      if (index!=0 && index!=0x80 && (!configuration ||
          (index!=BULK_OUT && index!=BULK_IN && index!=NOTIFY_IN))) { stall_control(); return; }
      ctrl[0]=(index&0x80) ? PCD->IN_ep[index&15].is_stall : PCD->OUT_ep[index&15].is_stall;
    }
    control_in(ctrl,2,length); return;
  } else if (bm==2 && (request==1 || request==3) && !value && !length && configuration &&
             (index==BULK_OUT || index==BULK_IN || index==NOTIFY_IN)) {
    if (request==3) (void)HAL_PCD_EP_SetStall(PCD,(uint8_t)index);
    else {
      (void)HAL_PCD_EP_ClrStall(PCD,(uint8_t)index);
      if (index==BULK_OUT) { rx_armed=0; arm_out(); }
      if (index==BULK_IN) { tx_busy=0; kick_in(); }
    }
    status_in(); return;
  } else if (bm==0x81 && request==10 && !value && index<2 && length==1 && configuration) {
    ctrl[0]=0; control_in(ctrl,1,length); return;
  } else if (bm==1 && request==11 && !value && index<2 && !length && configuration) {
    status_in(); return;
  } else if (bm==0xA1 && request==0x21 && !value && !index && length==7 && configuration) {
    control_in(line_coding,7,length); return;
  } else if (bm==0x21 && request==0x20 && !value && !index && length==7 && configuration) {
    ctrl_out=1; ctrl_stage=2;
    (void)HAL_PCD_EP_Receive(PCD,0,ctrl,7); return;
  } else if (bm==0x21 && request==0x22 && !index && !length && configuration && value<=3) {
    uint8_t next=(uint8_t)(value&1);
    if (next!=dtr) {
      dtr=next; reset_link();
    }
    status_in(); return;
  }
  stall_control();
}
void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *pcd, uint8_t ep)
{
  (void)pcd;
  if (ep==1) { tx_busy=0; kick_in(); return; }
  if (ep!=0) return;
  if (ctrl_stage==1) {
    if (ctrl_left) { next_control_in(); return; }
    if (ctrl_zlp) { ctrl_zlp=0; (void)HAL_PCD_EP_Transmit(PCD,0x80,ctrl,0); return; }
    ctrl_stage=4; (void)HAL_PCD_EP_Receive(PCD,0,ctrl,0);
  } else if (ctrl_stage==3) {
    ctrl_stage=0;
  }
}
void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *pcd, uint8_t ep)
{
  uint32_t n=HAL_PCD_EP_GetRxCount(pcd,ep);
  if (ep==0) {
    if (ctrl_stage==2 && ctrl_out && n==7) { memcpy(line_coding,ctrl,7); ctrl_out=0; status_in(); }
    else if (ctrl_stage==4) ctrl_stage=0;
    else stall_control();
    return;
  }
  if (ep!=1) return;
  rx_armed=0;
  if (!dtr || faulted) { arm_out(); return; }
  if (n<=64 && RX_SIZE-(rx_wr-rx_rd)>=n) {
    uint32_t wr=rx_wr, at=wr%RX_SIZE, first=n;
    if(first>RX_SIZE-at)first=RX_SIZE-at;
    memcpy(rx+at,out_packet,first); memcpy(rx,out_packet+first,n-first);
    __DMB(); rx_wr=wr+n;
  } else { faulted=1; ++epoch; }
  arm_out();
}
void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *pcd) { (void)pcd; suspended=1; }
void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *pcd) { (void)pcd; suspended=0; kick_in(); arm_out(); }
void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *pcd)
{ (void)pcd; configuration=dtr=0; reset_queues(); }
void USB_Device_Init(void)
{
  /* Preserve the previously working PLL3 USB clock. Generated PCD init has
   * already configured the embedded PHY and the interrupt. No USB DMA. */
  RCC_PeriphCLKInitTypeDef clock={0};
  clock.PeriphClockSelection=RCC_PERIPHCLK_USB;
  clock.PLL3.PLL3M=4; clock.PLL3.PLL3N=125;
  clock.PLL3.PLL3P=16; clock.PLL3.PLL3Q=16; clock.PLL3.PLL3R=8;
  clock.PLL3.PLL3RGE=RCC_PLL3VCIRANGE_2; clock.PLL3.PLL3VCOSEL=RCC_PLL3VCOWIDE;
  clock.UsbClockSelection=RCC_USBCLKSOURCE_PLL3;
  if (HAL_RCCEx_PeriphCLKConfig(&clock)!=HAL_OK) Error_Handler();
  HAL_NVIC_DisableIRQ(OTG_HS_IRQn);
  /* FIFO sizes in 32-bit words; total 256 words fits the core's FIFO RAM. */
  if (HAL_PCDEx_SetRxFiFo(PCD,128)!=HAL_OK ||
      HAL_PCDEx_SetTxFiFo(PCD,0,32)!=HAL_OK ||
      HAL_PCDEx_SetTxFiFo(PCD,1,64)!=HAL_OK ||
      HAL_PCDEx_SetTxFiFo(PCD,2,32)!=HAL_OK || HAL_PCD_Start(PCD)!=HAL_OK) Error_Handler();
  HAL_NVIC_EnableIRQ(OTG_HS_IRQn);
}
uint32_t USB_Device_Read(uint8_t *dst, uint32_t size)
{
  uint32_t saved=queue_lock();
  uint32_t n=rx_wr-rx_rd;
  if (n>size) n=size;
  uint32_t rd=rx_rd, at=rd%RX_SIZE, first=n;
  if(first>RX_SIZE-at)first=RX_SIZE-at;
  memcpy(dst,rx+at,first); memcpy(dst+first,rx,n-first);
  rx_rd=rd+n; arm_out();
  queue_unlock(saved);
  return n;
}
uint8_t USB_Device_Write(const uint8_t *src, uint32_t size)
{
  uint32_t saved=queue_lock();
  uint8_t ok=(uint8_t)(configuration && dtr && size<=TX_SIZE-(tx_wr-tx_rd));
  if (ok) {
    uint32_t wr=tx_wr, at=wr%TX_SIZE, first=size;
    if(first>TX_SIZE-at)first=TX_SIZE-at;
    memcpy(tx+at,src,first); memcpy(tx,src+first,size-first);
    tx_wr=wr+size; kick_in();
  }
  queue_unlock(saved);
  return ok;
}
uint32_t USB_Device_TxFree(void) { return TX_SIZE-(tx_wr-tx_rd); }
uint32_t USB_Device_Epoch(void) { return epoch; }
uint8_t USB_Device_Connected(void) { return (uint8_t)(configuration && dtr && !faulted && !suspended); }
void USB_Device_Fault(void)
{
  uint32_t saved=queue_lock();
  /* Let any already armed packet finish and then leave OUT NAKed. STALL/clear
   * would reset endpoint PID on only one side during DTR recovery. */
  faulted=1;
  queue_unlock(saved);
}

void USB_Device_Debug(char *text, uint32_t size)
{
  uint32_t gint=0, mask=0, ahb=0, cfg=0, ctl=0, otg=0, phy=0;
#if defined(__arm__) || defined(__thumb__)
  uint32_t USBx_BASE=(uint32_t)PCD->Instance;
  gint=PCD->Instance->GINTSTS; mask=PCD->Instance->GINTMSK;
  ahb=PCD->Instance->GAHBCFG; cfg=USBx_DEVICE->DCFG; ctl=USBx_DEVICE->DCTL;
  otg=PCD->Instance->GOTGCTL; phy=PCD->Instance->GCCFG;
#endif
  (void)snprintf(text,size,
    "ok: usbdev reset %lu setup %lu stall %lu state %u cfg %u dtr %u suspend %u fault %u "
    "last %02x%02x%02x%02x%02x%02x%02x%02x "
    "regs %08lx %08lx %08lx %08lx %08lx %08lx %08lx\r\n",
    (unsigned long)resets,(unsigned long)setups,(unsigned long)control_stalls,
    ctrl_stage,configuration,dtr,suspended,faulted,
    last_setup[0],last_setup[1],last_setup[2],last_setup[3],last_setup[4],last_setup[5],last_setup[6],last_setup[7],
    (unsigned long)gint,(unsigned long)mask,(unsigned long)ahb,(unsigned long)cfg,(unsigned long)ctl,
    (unsigned long)otg,(unsigned long)phy);
}
