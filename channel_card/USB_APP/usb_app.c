/* Binary-only CDC: BODY + chunked uploads, with controls/status on RS485. */
#include "usb_app.h"
#include "usb_device.h"
#include "usb_protocol.h"
#include "attack_upload.h"
#include "vm_upload.h"
#include "stream_ring.h"
#include "main.h"
#include <stdio.h>
#include <string.h>
static USB_Parser parser;
static uint32_t link_epoch, last_rx_ms, upload_ms;
static uint32_t rx_messages, rx_bytes, blocks, bad;
static uint8_t hello, failed, upload_kind, upload_target;
static uint32_t upload_total, upload_offset;
static uint32_t probe_bytes, probe_blocks, probe_hash;
static char upload_reply[96];
static uint8_t reply[USB_STREAM_HEADER_BYTES+128];
static uint16_t reply_size;

/* Existing upload validators report here; strings never enter the raw stream. */
void USB_CDC_WriteStr(const char *s)
{
  if (s) (void)snprintf(upload_reply,sizeof upload_reply,"%s",s);
}
static void abort_upload(void)
{
  VmUpload_Abort(); AttackUpload_Abort();
  upload_kind=0; upload_offset=upload_total=0;
}
static void make_reply(const uint8_t *header, const uint8_t *payload, uint16_t size)
{
  memcpy(reply,header,USB_STREAM_HEADER_BYTES);
  reply[0]=USB_MSG_REPLY;
  /* Request type occupies the reply's session byte; no request IDs are needed
   * because exactly one request/reply exchange may be outstanding. */
  reply[2]=header[0]; USB_Write16(reply+3,size);
  memcpy(reply+USB_STREAM_HEADER_BYTES,payload,size);
  reply_size=(uint16_t)(USB_STREAM_HEADER_BYTES+size);
}
static void result(const uint8_t *h, uint8_t error, const char *message)
{
  uint8_t data[96]; data[0]=error;
  size_t n=message ? strlen(message) : 0;
  if (n>sizeof(data)-1) n=sizeof(data)-1;
  if (n) memcpy(data+1,message,n);
  make_reply(h,data,(uint16_t)(n+1));
}
static void fail_link(const uint8_t *h, const char *message)
{
  ++bad; failed=1; hello=0; abort_upload();
  if (h) result(h,1,message);
  USB_Device_Fault();
}
static uint8_t dispatch(const uint8_t *h)
{
  uint16_t n=USB_Read16(h+3);
  const uint8_t *p=h+USB_STREAM_HEADER_BYTES;
  if (h[0]==USB_MSG_HELLO) {
    if (hello || n!=1 || p[0]!=USB_STREAM_VERSION || h[1] || h[2]) {
      fail_link(h,"incompatible HELLO"); return 1;
    }
    abort_upload();
    uint16_t processed=StreamRing_LastBodySequence();
    hello=1; probe_bytes=probe_blocks=0; probe_hash=2166136261u;
    const uint8_t caps[]={0,USB_STREAM_VERSION,8,1,0x80,0xBB,0,0,
                         USB_STREAM_PAYLOAD_MAX&255,USB_STREAM_PAYLOAD_MAX>>8,
                         USB_STREAM_PRIME_SAMPLES&255,USB_STREAM_PRIME_SAMPLES>>8,
                         processed&255,processed>>8};
    make_reply(h,caps,sizeof caps); return 1;
  }
  if (!hello) { fail_link(h,"HELLO required"); return 1; }
  /* Diagnostic sink: no voice, upload, or credit state is changed. An empty
   * probe is an ordered barrier returning totals for this connection. */
  if (h[0]==USB_MSG_PROBE) {
    if (h[1] || h[2]) { fail_link(h,"invalid PROBE"); return 1; }
    if (n) {
      probe_bytes+=n; ++probe_blocks;
      for (uint16_t i=0;i<n;++i) probe_hash=(probe_hash^p[i])*16777619u;
    } else {
      uint8_t stats[13]={0};
      uint32_t values[]={probe_bytes,probe_blocks,probe_hash};
      for (unsigned i=0;i<3;++i) {
        USB_Write16(stats+1+i*4,(uint16_t)values[i]);
        USB_Write16(stats+3+i*4,(uint16_t)(values[i]>>16));
      }
      make_reply(h,stats,sizeof stats);
    }
    return 1;
  }
  if (h[0]==USB_MSG_BODY) {
    if (!n || h[1]>=8 || h[2]>=USB_STREAM_SESSION_MOD) { fail_link(h,"invalid BODY"); return 1; }
    /* Prediction may temporarily run ahead of consumption. Retain this whole
     * block and apply USB backpressure; never overwrite or drop ring samples. */
    if (StreamRing_TargetSession(h[1])==h[2] && StreamRing_FreeLevel(h[1])<n) return 0;
    /* Stale notes are acknowledged/ignored by the ring; no stale data becomes
     * audible. Capacity errors are explicit, never silently dropped. */
    int accepted=StreamRing_WriteBody(h[1],h[2],(const int8_t *)p,n);
    if (accepted<0) { fail_link(h,"BODY ring capacity"); return 1; }
    rx_messages+=(uint32_t)(accepted>0); return 1;
  }
  if (h[2]) { fail_link(h,"invalid upload header"); return 1; }
  upload_reply[0]=0;
  if (h[0]==USB_MSG_UPLOAD_BEGIN) {
    if (n!=5 || upload_kind) { result(h,1,"upload busy/invalid"); return 1; }
    uint32_t size=USB_Read32(p+1);
    int rc=-1;
    if (p[0]==USB_UPLOAD_ATTACK) rc=AttackUpload_Begin(h[1],size);
    else if (p[0]==USB_UPLOAD_WAVE) rc=AttackUpload_BeginWavetable(h[1],size);
    else if (p[0]==USB_UPLOAD_SCRIPT) rc=VmUpload_Begin(h[1],size);
    if (rc) { result(h,1,"upload rejected (target, size, or active voice)"); return 1; }
    upload_kind=p[0]; upload_target=h[1]; upload_total=size; upload_offset=0;
    upload_ms=HAL_GetTick(); result(h,0,NULL); return 1;
  }
  if (h[0]==USB_MSG_UPLOAD_DATA) {
    if (!upload_kind || h[1]!=upload_target || n<=4 || USB_Read32(p)!=upload_offset ||
        (uint32_t)(n-4)>upload_total-upload_offset) {
      abort_upload(); result(h,1,"upload offset/length"); return 1;
    }
    uint32_t take=upload_kind==USB_UPLOAD_SCRIPT ? VmUpload_Feed(p+4,n-4) : AttackUpload_Feed(p+4,n-4);
    if (take!=(uint32_t)(n-4) || strncmp(upload_reply,"err:",4)==0) {
      result(h,1,upload_reply[0] ? upload_reply : "upload failed"); abort_upload(); return 1;
    }
    upload_offset+=take; upload_ms=HAL_GetTick();
    result(h,0,NULL);
    if (upload_offset==upload_total) abort_upload();
    return 1;
  }
  if (h[0]==USB_MSG_UPLOAD_ABORT && n==0) { abort_upload(); result(h,0,NULL); return 1; }
  fail_link(h,"unknown request"); return 1;
}
void USB_App_Init(void)
{
  USB_ParserReset(&parser); USB_Device_Init(); link_epoch=USB_Device_Epoch();
}
void USB_App_Task(void)
{
  uint32_t epoch=USB_Device_Epoch();
  if (epoch!=link_epoch) {
    abort_upload(); USB_ParserReset(&parser); hello=failed=0; reply_size=0;
    link_epoch=epoch;
  }
  if (reply_size) {
    if (!USB_Device_Write(reply,reply_size)) return;
    reply_size=0;
  }
  if (!USB_Device_Connected() || failed) return;
  if (parser.used==parser.need) {
    if (!dispatch(parser.bytes)) return;
    USB_ParserReset(&parser);
    if (failed || reply_size) return;
  }
  /* Bounded passes keep the main-loop RS485 service responsive. */
  uint32_t budget=4096;
  uint8_t bytes[64];
  while (budget && !reply_size && !failed) {
    /* Never consume beyond this frame: a pending reply must not discard the
     * beginning of the next block from the same USB packet. */
    uint32_t want=parser.need-parser.used;
    if (want>sizeof bytes) want=sizeof bytes;
    if (want>budget) want=budget;
    uint32_t n=USB_Device_Read(bytes,want);
    if (USB_Device_Epoch()!=link_epoch) return;
    if (!n) break;
    rx_bytes+=n; budget-=n; last_rx_ms=HAL_GetTick();
    for (uint32_t i=0;i<n;++i) {
      int rc=USB_ParserByte(&parser,bytes[i]);
      if (rc<0) { fail_link(NULL,"bad header"); return; }
      if (rc>0) { ++blocks; if (dispatch(parser.bytes)) USB_ParserReset(&parser); }
    }
  }
  if (parser.used && parser.used<parser.need && (uint32_t)(HAL_GetTick()-last_rx_ms)>1000) fail_link(NULL,"partial frame timeout");
  if (upload_kind && (uint32_t)(HAL_GetTick()-upload_ms)>5000) abort_upload();
}
uint16_t USB_App_LastPackSequence(void) { return StreamRing_LastBodySequence(); }
uint32_t USB_App_RxMsgCount(void) { return rx_messages; }
uint32_t USB_App_RxByteCount(void) { return rx_bytes; }
uint32_t USB_App_BlockCount(void) { return blocks; }
uint32_t USB_App_BadCount(void) { return bad; }
uint32_t USB_App_BadReasonCount(uint8_t reason) { return reason==4 ? bad : 0; }
void USB_App_StatsClear(void) { rx_messages=rx_bytes=blocks=bad=0; }
