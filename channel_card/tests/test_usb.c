#include "usb_otg.h"
#include "usb_device.h"
#include "usb_app.h"
#include "usb_stream.h"
#include "stream_ring.h"
#include "attack_bank.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
PCD_HandleTypeDef hpcd_USB_OTG_HS;
static uint32_t tick, rx_count[2], rx_length[2], tx_length[3];
static uint8_t *rx_buffer[2], tx_copy[3][64], address;
static uint8_t reply_bytes[8192];
static unsigned reply_count, data_opens;
uint32_t HAL_GetTick(void) { return tick; }
uint32_t HAL_GetUIDw0(void) { return 1; }
uint32_t HAL_GetUIDw1(void) { return 2; }
uint32_t HAL_GetUIDw2(void) { return 3; }
int HAL_RCCEx_PeriphCLKConfig(RCC_PeriphCLKInitTypeDef *p) { (void)p; return 0; }
void HAL_NVIC_DisableIRQ(int irq) { (void)irq; }
void HAL_NVIC_EnableIRQ(int irq) { (void)irq; }
void Error_Handler(void) { CHECK(0); }
int HAL_PCD_EP_Transmit(PCD_HandleTypeDef *p,uint8_t ep,uint8_t *data,uint32_t n) {
  (void)p; ep &= 15; CHECK(n<=64); tx_length[ep]=n;
  memcpy(tx_copy[ep],data,n);
  if (ep==1) { CHECK(reply_count+n<=sizeof reply_bytes); memcpy(reply_bytes+reply_count,data,n); reply_count+=n; }
  return 0;
}
int HAL_PCD_EP_Receive(PCD_HandleTypeDef *p,uint8_t ep,uint8_t *data,uint32_t n) {
  (void)p; CHECK(ep<2); rx_buffer[ep]=data; rx_length[ep]=n; return 0;
}
int HAL_PCD_EP_Open(PCD_HandleTypeDef *p,uint8_t ep,uint16_t size,uint8_t type) {
  if(ep==1 || ep==0x81) ++data_opens;
  (void)p; CHECK((type==EP_TYPE_INTR && size==8) || size==64); (void)ep; return 0;
}
int HAL_PCD_EP_Close(PCD_HandleTypeDef *p,uint8_t ep) { (void)p; if (!(ep&128)) rx_buffer[ep]=NULL; return 0; }
int HAL_PCD_EP_Abort(PCD_HandleTypeDef *p,uint8_t ep) { (void)p;CHECK(ep==0x81);return 0; }
int HAL_PCD_EP_Flush(PCD_HandleTypeDef *p,uint8_t ep) { (void)p;CHECK(ep==0x81);return 0; }
int HAL_PCD_EP_SetStall(PCD_HandleTypeDef *p,uint8_t ep) {
  if (ep&128) p->IN_ep[ep&15].is_stall=1; else p->OUT_ep[ep].is_stall=1; return 0;
}
int HAL_PCD_EP_ClrStall(PCD_HandleTypeDef *p,uint8_t ep) {
  if (ep&128) p->IN_ep[ep&15].is_stall=0; else p->OUT_ep[ep].is_stall=0; return 0;
}
int HAL_PCD_SetAddress(PCD_HandleTypeDef *p,uint8_t a) { (void)p; address=a; return 0; }
int HAL_PCDEx_SetRxFiFo(PCD_HandleTypeDef *p,uint16_t n) { (void)p; CHECK(n==128); return 0; }
int HAL_PCDEx_SetTxFiFo(PCD_HandleTypeDef *p,uint8_t ep,uint16_t n) { (void)p; (void)ep; CHECK(n>=16); return 0; }
int HAL_PCD_Start(PCD_HandleTypeDef *p) { (void)p; return 0; }
uint32_t HAL_PCD_EP_GetRxCount(PCD_HandleTypeDef *p,uint8_t ep) { (void)p; return rx_count[ep]; }
/* VM container validation has its own runtime tests; this transport test stubs
 * only that consumer, while exercising real attack uploads and BODY rings. */
static uint8_t vm_active;
int VmUpload_Begin(uint8_t v,uint32_t n) { if(v>=8 || n<20 || n>16404) return -1; vm_active=1; return 0; }
void VmUpload_Abort(void) { vm_active=0; }
uint8_t VmUpload_IsActive(void) { return vm_active; }
uint32_t VmUpload_Feed(const uint8_t *data,uint32_t n) { (void)data; return vm_active?n:0; }
uint8_t NoteBank_AnyBankReferences(void) { return 0; }
static void setup(uint8_t bm,uint8_t req,uint16_t value,uint16_t index,uint16_t length) {
  uint8_t *s=(uint8_t*)hpcd_USB_OTG_HS.Setup;
  s[0]=bm;s[1]=req;USB_Write16(s+2,value);USB_Write16(s+4,index);USB_Write16(s+6,length);
  HAL_PCD_SetupStageCallback(&hpcd_USB_OTG_HS);
}
static void in_done(uint8_t ep) { HAL_PCD_DataInStageCallback(&hpcd_USB_OTG_HS,ep); }
static void out(const uint8_t *bytes,unsigned n) {
  CHECK(rx_buffer[1] && n<=rx_length[1]); memcpy(rx_buffer[1],bytes,n); rx_count[1]=n;
  rx_buffer[1]=NULL; HAL_PCD_DataOutStageCallback(&hpcd_USB_OTG_HS,1);
}
static void flush_reply(void) {
  for (unsigned i=0;i<32;++i) { USB_App_Task(); in_done(1); }
}
static void frame(uint8_t type,uint8_t target,uint8_t session,uint8_t flags,uint16_t seq,const uint8_t *p,uint16_t n) {
  uint8_t bytes[1032]={type,target,session,flags,0,0,0,0};
  USB_Write16(bytes+4,n);USB_Write16(bytes+6,seq); if(n)memcpy(bytes+8,p,n);
  /* Adversarial 3-byte fragments, including frames much larger than USB packets. */
  for(unsigned at=0;at<8u+n;) { unsigned count=8u+n-at; if(count>3) count=3;
    out(bytes+at,count); at+=count; USB_App_Task(); }
  flush_reply();
}
static void connect(void) {
  setup(0,9,1,0,0);in_done(0);setup(0x21,0x22,1,0,0);in_done(0);USB_App_Task();
  reply_count=0;uint8_t version=1;frame(USB_MSG_HELLO,0,0,0,1,&version,1);
  CHECK(reply_count==20 && reply_bytes[0]==USB_MSG_REPLY && reply_bytes[8]==0);
  CHECK(reply_bytes[9]==1 && USB_Read16(reply_bytes+16)==1024);
  reply_count=0;
}
static void enumeration(void) {
  HAL_PCD_ResetCallback(&hpcd_USB_OTG_HS);
  setup(0x80,6,0x0100,0,18); CHECK(tx_length[0]==18 && tx_copy[0][4]==0xEF);in_done(0);
  rx_count[0]=0;HAL_PCD_DataOutStageCallback(&hpcd_USB_OTG_HS,0);
  setup(0x80,6,0x0200,0,255); CHECK(tx_length[0]==64 && tx_copy[0][2]==75);in_done(0);
  CHECK(tx_length[0]==11);in_done(0);CHECK(rx_length[0]==0);
  rx_count[0]=0;HAL_PCD_DataOutStageCallback(&hpcd_USB_OTG_HS,0);
  setup(0,5,42,0,0);CHECK(address==42 && tx_length[0]==0);in_done(0);
  connect();
  setup(0x21,0x20,0,0,7);CHECK(rx_length[0]==7);
  uint8_t coding[]={0,0xC2,1,0,0,0,8};memcpy(rx_buffer[0],coding,7);rx_count[0]=7;
  HAL_PCD_DataOutStageCallback(&hpcd_USB_OTG_HS,0);CHECK(tx_length[0]==0);in_done(0);
  setup(0xA1,0x21,0,0,7);CHECK(tx_length[0]==7 && memcmp(tx_copy[0],coding,7)==0);in_done(0);
}
static void app_test(void) {
  uint8_t data[1024];for(unsigned i=0;i<sizeof data;++i)data[i]=(uint8_t)i;
  StreamRing_ArmPending(0,0,1);
  frame(USB_MSG_BODY,0,1,1,1,data,998);CHECK(StreamRing_PendingFill(0)==998 && reply_count==0);
  CHECK(StreamRing_StartNote(0)==0);
  uint8_t begin[]={USB_UPLOAD_ATTACK,0,2,0,0}; // 512-byte attack
  frame(USB_MSG_UPLOAD_BEGIN,1,0,0,2,begin,5);CHECK(reply_bytes[8]==0);reply_count=0;
  frame(USB_MSG_BODY,0,1,0,2,data,1024);CHECK(StreamRing_CurrentFill(0)==2022);
  uint8_t chunk[516]={0};memcpy(chunk+4,data,512);
  frame(USB_MSG_UPLOAD_DATA,1,0,0,3,chunk,516);CHECK(reply_bytes[8]==0 && AttackBank_IsLoaded(1));
  CHECK(memcmp(AttackBank_Table(1),data,512)==0);reply_count=0;
  // Wrong offset cannot commit a partial upload.
  frame(USB_MSG_UPLOAD_BEGIN,2,0,0,4,begin,5);reply_count=0;
  chunk[0]=1;frame(USB_MSG_UPLOAD_DATA,2,0,0,5,chunk,516);
  CHECK(reply_bytes[8]!=0 && !AttackBank_IsLoaded(2));reply_count=0;
  // Frame truncation faults transport instead of interpreting payload as headers.
  uint8_t truncated[]={USB_MSG_BODY,0,1,0,32,0,3,0};out(truncated,8);USB_App_Task();
  tick+=1001;USB_App_Task();CHECK(!USB_Device_Connected());
  unsigned opens=data_opens;
  setup(0x21,0x22,0,0,0);in_done(0);setup(0x21,0x22,1,0,0);in_done(0);USB_App_Task();
  CHECK(USB_Device_Connected() && data_opens==opens);
  uint8_t ver=1;frame(USB_MSG_HELLO,0,0,0,6,&ver,1);CHECK(reply_bytes[8]==0);
}
static void probe_test(void) {
  reply_count=0;
  uint8_t data[1024]; uint32_t hash=2166136261u;
  for(unsigned i=0;i<1024;++i) { data[i]=(uint8_t)i; hash=(hash^data[i])*16777619u; }
  uint16_t seq=StreamRing_LastBodySequence(); uint32_t fill=StreamRing_CurrentFill(0);
  frame(USB_MSG_PROBE,0,0,0,7,data,sizeof data);CHECK(reply_count==0);
  frame(USB_MSG_PROBE,0,0,0,8,NULL,0);
  CHECK(reply_count==21 && reply_bytes[8]==0 && reply_bytes[3]==USB_MSG_PROBE);
  CHECK(USB_Read32(reply_bytes+9)==1024 && USB_Read32(reply_bytes+13)==1);
  CHECK(USB_Read32(reply_bytes+17)==hash);
  CHECK(StreamRing_LastBodySequence()==seq && StreamRing_CurrentFill(0)==fill);
  setup(0x21,0x22,0,0,0);in_done(0);connect();
  frame(USB_MSG_PROBE,0,0,0,9,NULL,0);
  CHECK(USB_Read32(reply_bytes+9)==0 && USB_Read32(reply_bytes+13)==0);
  CHECK(USB_Read32(reply_bytes+17)==2166136261u);
}
static void body_backpressure(void) {
  StreamRing_Init();setup(0x21,0x22,0,0,0);in_done(0);connect();
  uint8_t data[1024];memset(data,0x35,sizeof data);StreamRing_ArmPending(0,0,1);
  frame(USB_MSG_BODY,0,1,1,1,data,998);CHECK(StreamRing_StartNote(0)==0);
  for(uint16_t seq=2;seq<=4;++seq)frame(USB_MSG_BODY,0,1,0,seq,data,1024);
  CHECK(StreamRing_CurrentFill(0)==4070);
  frame(USB_MSG_BODY,0,1,0,5,data,1024);
  CHECK(StreamRing_LastBodySequence()==4 && StreamRing_FullCount()==0 && USB_Device_Connected());
  StreamRing_Advance(0,1024);flush_reply();
  CHECK(StreamRing_LastBodySequence()==5 && StreamRing_CurrentFill(0)==4070);
  CHECK(StreamRing_FullCount()==0 && reply_count==0);
}
static void backpressure(void) {
  setup(0x21,0x22,0,0,0);in_done(0);setup(0x21,0x22,1,0,0);in_done(0);
  uint8_t data[64]; memset(data,0xA5,64);
  for(unsigned i=0;i<128;++i)out(data,64);
  CHECK(rx_buffer[1]==NULL); // OUT stays NAKed until storage is available.
  uint8_t readback[64];CHECK(USB_Device_Read(readback,64)==64 && rx_buffer[1]);
  CHECK(memcmp(readback,data,64)==0);out(data,64);CHECK(rx_buffer[1]==NULL);
  for(unsigned i=0;i<128;++i)CHECK(USB_Device_Read(readback,64)==64);
  CHECK(USB_Device_Read(readback,64)==0);
  reply_count=0;CHECK(USB_Device_Write(data,64));CHECK(tx_length[1]==64);in_done(1);
  CHECK(tx_length[1]==0);in_done(1);CHECK(reply_count==64); // Terminating ZLP, no added data byte.
}
int main(void) {
  StreamRing_Init();AttackBank_Init();USB_App_Init();enumeration();app_test();probe_test();body_backpressure();backpressure();
  puts("CDC enumeration, control transfers, uploads/BODY interleaving, reconnect and backpressure passed");return 0;
}
