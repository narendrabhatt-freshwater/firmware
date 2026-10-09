#include "usb.h"
#include "channel.h"
#include "samples.h"
#include "stream.h"
#include "usb_otg.h"
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
  (void)p; CHECK(ep<2); rx_buffer[ep]=data; rx_length[ep]=n; rx_count[ep]=0; return 0;
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
/* Model HAL's per-packet FIFO copy separately from transfer completion. Full
 * packets leave the 512-byte receive armed; a short packet or full transfer
 * invokes the completion callback. */
static void packet(const uint8_t *bytes,unsigned n) {
  CHECK(rx_buffer[1] && n<=64 && rx_count[1]+n<=rx_length[1]);
  memcpy(rx_buffer[1]+rx_count[1],bytes,n); rx_count[1]+=n;
  if(n<64 || rx_count[1]==rx_length[1]) {
    rx_buffer[1]=NULL;HAL_PCD_DataOutStageCallback(&hpcd_USB_OTG_HS,1);
  }
}
static void flush_reply(void) {
  for (unsigned i=0;i<32;++i) { USB_App_Task(); in_done(1); }
}
static void frame(uint8_t type,uint8_t target,uint8_t session,const uint8_t *p,uint16_t n) {
  uint8_t bytes[USB_STREAM_HEADER_BYTES+USB_STREAM_PAYLOAD_MAX]={type,target,session,0,0};
  USB_Write16(bytes+3,n); if(n)memcpy(bytes+USB_STREAM_HEADER_BYTES,p,n);
  /* Adversarial 3-byte fragments, including frames much larger than USB packets. */
  for(unsigned at=0;at<USB_STREAM_HEADER_BYTES+n;) { unsigned count=USB_STREAM_HEADER_BYTES+n-at; if(count>3) count=3;
    out(bytes+at,count); at+=count; USB_App_Task(); }
  flush_reply();
}
static void connect(void) {
  setup(0,9,1,0,0);in_done(0);setup(0x21,0x22,1,0,0);in_done(0);USB_App_Task();
  reply_count=0;uint8_t version=USB_STREAM_VERSION;frame(USB_MSG_HELLO,0,0,&version,1);
  CHECK(reply_count==19 && reply_bytes[0]==USB_MSG_REPLY && reply_bytes[5]==0);
  CHECK(reply_bytes[6]==USB_STREAM_VERSION && USB_Read16(reply_bytes+13)==1024);
  CHECK(USB_Read16(reply_bytes+17)==StreamRing_LastBodySequence());
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
  frame(USB_MSG_BODY,0,1,data,998);CHECK(StreamRing_PendingFill(0)==998 && reply_count==0);
  CHECK(StreamRing_StartNote(0)==0);
  uint8_t begin[]={USB_UPLOAD_ATTACK,0,2,0,0}; // 512-byte attack
  frame(USB_MSG_UPLOAD_BEGIN,1,0,begin,5);CHECK(reply_bytes[5]==0);reply_count=0;
  frame(USB_MSG_BODY,0,1,data,1024);CHECK(StreamRing_CurrentFill(0)==2022);
  uint8_t chunk[516]={0};memcpy(chunk+4,data,512);
  frame(USB_MSG_UPLOAD_DATA,1,0,chunk,516);CHECK(reply_bytes[5]==0 && AttackBank_IsLoaded(1));
  CHECK(memcmp(AttackBank_Table(1),data,512)==0);reply_count=0;
  // Wrong offset cannot commit a partial upload.
  frame(USB_MSG_UPLOAD_BEGIN,2,0,begin,5);reply_count=0;
  chunk[0]=1;frame(USB_MSG_UPLOAD_DATA,2,0,chunk,516);
  CHECK(reply_bytes[5]!=0 && !AttackBank_IsLoaded(2));reply_count=0;
  // Frame truncation faults transport instead of interpreting payload as headers.
  uint8_t truncated[]={USB_MSG_BODY,0,1,32,0};out(truncated,sizeof truncated);USB_App_Task();
  tick+=1001;USB_App_Task();CHECK(!USB_Device_Connected());
  unsigned opens=data_opens;
  setup(0x21,0x22,0,0,0);in_done(0);setup(0x21,0x22,1,0,0);in_done(0);USB_App_Task();
  CHECK(USB_Device_Connected() && data_opens==opens);
  uint8_t ver=USB_STREAM_VERSION;frame(USB_MSG_HELLO,0,0,&ver,1);CHECK(reply_bytes[5]==0);
}
static void probe_test(void) {
  reply_count=0;
  uint8_t data[1024]; uint32_t hash=2166136261u;
  for(unsigned i=0;i<1024;++i) { data[i]=(uint8_t)i; hash=(hash^data[i])*16777619u; }
  uint16_t seq=StreamRing_LastBodySequence(); uint32_t fill=StreamRing_CurrentFill(0);
  frame(USB_MSG_PROBE,0,0,data,sizeof data);CHECK(reply_count==0);
  frame(USB_MSG_PROBE,0,0,NULL,0);
  CHECK(reply_count==18 && reply_bytes[5]==0 && reply_bytes[2]==USB_MSG_PROBE);
  CHECK(USB_Read32(reply_bytes+6)==1024 && USB_Read32(reply_bytes+10)==1);
  CHECK(USB_Read32(reply_bytes+14)==hash);
  CHECK(StreamRing_LastBodySequence()==seq && StreamRing_CurrentFill(0)==fill);
  setup(0x21,0x22,0,0,0);in_done(0);connect();
  frame(USB_MSG_PROBE,0,0,NULL,0);
  CHECK(USB_Read32(reply_bytes+6)==0 && USB_Read32(reply_bytes+10)==0);
  CHECK(USB_Read32(reply_bytes+14)==2166136261u);
}
static void body_backpressure(void) {
  StreamRing_Init();setup(0x21,0x22,0,0,0);in_done(0);connect();
  uint8_t data[1024];memset(data,0x35,sizeof data);StreamRing_ArmPending(0,0,1);
  frame(USB_MSG_BODY,0,1,data,998);CHECK(StreamRing_StartNote(0)==0);
  for(uint16_t seq=2;seq<=4;++seq)frame(USB_MSG_BODY,0,1,data,1024);
  CHECK(StreamRing_CurrentFill(0)==4070);
  frame(USB_MSG_BODY,0,1,data,1024);
  CHECK(StreamRing_LastBodySequence()==4 && StreamRing_FullCount()==0 && USB_Device_Connected());
  StreamRing_Advance(0,1024);flush_reply();
  CHECK(StreamRing_LastBodySequence()==5 && StreamRing_CurrentFill(0)==4070);
  CHECK(StreamRing_FullCount()==0 && reply_count==0);
  // Reopen while a complete BODY is blocked. It and subsequent queued bytes
  // are discarded; HELLO must report only the five blocks actually processed.
  frame(USB_MSG_BODY,0,1,data,1024);
  frame(USB_MSG_BODY,0,1,data,20);
  CHECK(StreamRing_LastBodySequence()==5);
  setup(0x21,0x22,0,0,0);in_done(0);connect();
  CHECK(StreamRing_LastBodySequence()==5 && StreamRing_CurrentFill(0)==4070);
  StreamRing_Advance(0,1024);
  frame(USB_MSG_BODY,0,1,data,1024);
  CHECK(StreamRing_LastBodySequence()==6 && StreamRing_CurrentFill(0)==4070);
  CHECK(StreamRing_FullCount()==0 && reply_count==0);
}
static void session_and_counter_test(void) {
  StreamRing_Init();StreamRing_StatsClear();
  setup(0x21,0x22,0,0,0);in_done(0);connect();
  uint8_t data[1024];memset(data,0x27,sizeof data);
  StreamRing_ArmPending(0,0,254);
  frame(USB_MSG_BODY,0,254,data,256);
  frame(USB_MSG_BODY,0,254,data,742);
  CHECK(StreamRing_PendingFill(0)==998 && StreamRing_SofCount()==1);
  CHECK(StreamRing_StartNote(0)==0);
  StreamRing_ArmPending(0,1,0);
  frame(USB_MSG_BODY,0,254,data,20);
  CHECK(StreamRing_LastBodySequence()==3 && StreamRing_PendingFill(0)==0 && StreamRing_CurrentFill(0)==1018);
  frame(USB_MSG_BODY,0,0,data,998);
  CHECK(StreamRing_SofCount()==2 && StreamRing_PendingFill(0)==998);
  CHECK(StreamRing_StartNote(0)==0);
  StreamRing_ResetAll();frame(USB_MSG_BODY,0,0,data,20);
  CHECK(StreamRing_LastBodySequence()==5 && StreamRing_FillLevel(0)==0);
  // Partial header on disconnect cannot advance the cumulative counter.
  const uint8_t partial[]={USB_MSG_BODY,0};out(partial,sizeof partial);USB_App_Task();
  setup(0x21,0x22,0,0,0);in_done(0);connect();
  CHECK(StreamRing_LastBodySequence()==5);
  frame(USB_MSG_BODY,0,0,data,1); // A retired block is still processed exactly once.
  CHECK(StreamRing_LastBodySequence()==6);
  // All voices share the same block counter; they do not reset it on note-on.
  for(uint8_t v=0;v<8;++v) {
    StreamRing_ArmPending(v,v,7);frame(USB_MSG_BODY,v,7,data,998);
    CHECK(StreamRing_PendingFill(v)==998);
  }
  CHECK(StreamRing_LastBodySequence()==14);
  StreamRing_ResetAll();
  for(unsigned i=14;i<65536u;++i)frame(USB_MSG_BODY,0,7,data,1);
  CHECK(StreamRing_LastBodySequence()==0);
  frame(USB_MSG_BODY,0,7,data,1);CHECK(StreamRing_LastBodySequence()==1);
  CHECK(StreamRing_DropCount()==0 && reply_count==0);
}
static void incompatible_test(void) {
  setup(0x21,0x22,0,0,0);in_done(0);
  setup(0x21,0x22,1,0,0);in_done(0);USB_App_Task();
  uint8_t old_version=1;reply_count=0;
  frame(USB_MSG_HELLO,0,0,&old_version,1);
  CHECK(!USB_Device_Connected() && reply_bytes[5]==1);
  setup(0x21,0x22,0,0,0);in_done(0);
  setup(0x21,0x22,1,0,0);in_done(0);USB_App_Task();
  const uint8_t old_hello[]={1,0,0,0,1,0,1,0,1};
  out(old_hello,sizeof old_hello);USB_App_Task();tick+=1001;USB_App_Task();
  CHECK(!USB_Device_Connected());
  setup(0x21,0x22,0,0,0);in_done(0);connect();
}
static void backpressure(void) {
  setup(0x21,0x22,0,0,0);in_done(0);setup(0x21,0x22,1,0,0);in_done(0);
  uint8_t data[64]; memset(data,0xA5,64);
  for(unsigned i=0;i<128;++i)packet(data,64);
  CHECK(rx_buffer[1]==NULL); // OUT stays NAKed until storage is available.
  uint8_t readback[64];
  for(unsigned i=0;i<8;++i) {
    CHECK(USB_Device_Read(readback,64)==64);
    CHECK(memcmp(readback,data,64)==0);
    CHECK((rx_buffer[1]!=NULL)==(i==7)); // Reserve an entire receive transfer.
  }
  for(unsigned i=0;i<8;++i)packet(data,64);
  CHECK(rx_buffer[1]==NULL);
  for(unsigned i=0;i<128;++i)CHECK(USB_Device_Read(readback,64)==64);
  CHECK(USB_Device_Read(readback,64)==0);
  reply_count=0;CHECK(USB_Device_Write(data,64));CHECK(tx_length[1]==64);in_done(1);
  CHECK(tx_length[1]==0);in_done(1);CHECK(reply_count==64); // Terminating ZLP, no added data byte.
}
static void partial_transfer_test(void) {
  setup(0x21,0x22,0,0,0);in_done(0);connect();USB_App_StatsClear();
  uint8_t message[64]={USB_MSG_PROBE,0,0,59,0};
  uint32_t hash=2166136261u;
  for(unsigned i=5;i<64;++i) {message[i]=(uint8_t)i;hash=(hash^message[i])*16777619u;}
  packet(message,64);CHECK(rx_count[1]==64);USB_App_Task();
  CHECK(USB_App_BlockCount()==1 && USB_App_RxByteCount()==64);
  CHECK(rx_count[1]==64); // Parsed without completion, a short packet or ZLP.
  for(unsigned i=1;i<8;++i) {
    packet(message,64);USB_App_Task();
    for(unsigned j=5;j<64;++j)hash=(hash^message[j])*16777619u;
  }
  CHECK(rx_count[1]==0 && USB_App_BlockCount()==8); // Completion did not duplicate data.
  const uint8_t barrier[]={USB_MSG_PROBE,0,0,0,0};
  packet(barrier,5);flush_reply();
  CHECK(reply_count==18 && USB_Read32(reply_bytes+6)==8*59);
  CHECK(USB_Read32(reply_bytes+10)==8 && USB_Read32(reply_bytes+14)==hash);
  // Drop unpublished old bytes when DTR changes, preserving the in-flight
  // transfer and USB packet toggles. New HELLO appends to that same buffer.
  packet(message,64);
  unsigned opens=data_opens;
  setup(0x21,0x22,0,0,0);in_done(0);
  packet(message,64);
  setup(0x21,0x22,1,0,0);in_done(0);USB_App_Task();reply_count=0;
  const uint8_t hello[]={USB_MSG_HELLO,0,0,1,0,USB_STREAM_VERSION};
  packet(hello,sizeof hello);flush_reply();
  CHECK(data_opens==opens && USB_Device_Connected());
  CHECK(reply_count==19 && reply_bytes[5]==0);
}
static void burst_stream_test(void) {
  setup(0x21,0x22,0,0,0);in_done(0);connect();USB_App_StatsClear();
  /* Cross packet, 512-byte receive, parser and 8192-byte queue boundaries with
   * different payloads, while allowing multiple packets between task calls. */
  uint8_t wire[40*(USB_STREAM_HEADER_BYTES+1024)];
  uint32_t hash=2166136261u;
  for(unsigned b=0;b<40;++b) {
    uint8_t *p=wire+b*(USB_STREAM_HEADER_BYTES+1024);
    p[0]=USB_MSG_PROBE;p[1]=p[2]=0;USB_Write16(p+3,1024);
    for(unsigned i=0;i<1024;++i) {
      p[5+i]=(uint8_t)(i+b);hash=(hash^p[5+i])*16777619u;
    }
  }
  unsigned packets=0;
  for(unsigned at=0;at<sizeof wire;) {
    unsigned n=sizeof wire-at;if(n>64)n=64;
    packet(wire+at,n);at+=n;
    if(++packets%13==0)USB_App_Task();
  }
  flush_reply();CHECK(reply_count==0 && USB_App_RxByteCount()==sizeof wire);
  CHECK(USB_App_BlockCount()==40 && USB_Device_Connected());
  const uint8_t barrier[]={USB_MSG_PROBE,0,0,0,0};
  packet(barrier,5);flush_reply();
  CHECK(reply_count==18 && USB_Read32(reply_bytes+6)==40*1024);
  CHECK(USB_Read32(reply_bytes+10)==40 && USB_Read32(reply_bytes+14)==hash);
}
int main(void) {
  StreamRing_Init();AttackBank_Init();USB_App_Init();enumeration();app_test();probe_test();partial_transfer_test();burst_stream_test();body_backpressure();session_and_counter_test();incompatible_test();backpressure();
  puts("CDC enumeration, control transfers, uploads/BODY interleaving, reconnect and backpressure passed");return 0;
}
