#include "stream_ring.h"
#include "usb_stream.h"
#include <stdio.h>
#include <stdlib.h>

static void Check(int ok, const char *message)
{ if (!ok) { fprintf(stderr, "%s\n", message); exit(EXIT_FAILURE); } }
static void Fill(int8_t *samples, int8_t origin)
{ uint32_t i; for (i=0u;i<USB_STREAM_UAC_BODY_SAMPLES;++i) samples[i]=(int8_t)(origin+(int8_t)(i%20u)); }

int main(void)
{
  int8_t old_body[USB_STREAM_UAC_BODY_SAMPLES];
  int8_t new_body[USB_STREAM_UAC_BODY_SAMPLES];
  int8_t newer_body[USB_STREAM_UAC_BODY_SAMPLES];
  int8_t sample=0;
  StreamRing_Write_t write;
  int8_t packet[USB_STREAM_UAC_PACKET_BYTES] = {0};
  Fill(old_body,10); Fill(new_body,40); Fill(newer_body,70);
  StreamRing_Init();
  Check(STREAM_RING_SAMPLES==4080u,"8-bit ring capacity must be 85 ms");
  Check(StreamRing_WriteBegin(0u,1u,1u,10u,USB_STREAM_UAC_BODY_SAMPLES,&write)==STREAM_RING_WRITE_FUTURE,"SOF before authority must be future");
  StreamRing_ArmPending(0u,10u,1u);
  Check(StreamRing_WriteVoice(0u,1u,1u,10u,old_body,USB_STREAM_UAC_BODY_SAMPLES)==USB_STREAM_UAC_BODY_SAMPLES,"pending BODY must publish");
  Check(StreamRing_CurrentFill(0u)==0u&&StreamRing_PendingFill(0u)==USB_STREAM_UAC_BODY_SAMPLES&&StreamRing_GetRel(0u,0u,&sample)!=0,"current must never read pending");
  Check(StreamRing_StartNote(0u)==0&&StreamRing_GetRel(0u,0u,&sample)==0&&sample==10,"promotion must start at pending origin");
  StreamRing_Advance(0u,20u);
  StreamRing_ArmPending(0u,11u,2u);
  Check(StreamRing_WriteVoice(0u,2u,1u,11u,new_body,USB_STREAM_UAC_BODY_SAMPLES)==USB_STREAM_UAC_BODY_SAMPLES,"pending fills while current drains");
  Check(StreamRing_CurrentFill(0u)==USB_STREAM_UAC_BODY_SAMPLES-20u&&StreamRing_PendingFill(0u)==USB_STREAM_UAC_BODY_SAMPLES&&StreamRing_GetRel(0u,0u,&sample)==0&&sample==10,"pending must preserve current");
  StreamRing_ArmPending(0u,12u,3u);
  Check(StreamRing_PendingFill(0u)==0u&&StreamRing_GetRel(0u,0u,&sample)==0&&sample==10,"superseding pending preserves current");
  Check(StreamRing_WriteVoice(0u,2u,1u,11u,new_body,USB_STREAM_UAC_BODY_SAMPLES)==0u,"superseded session rejected");
  Check(StreamRing_WriteVoice(0u,1u,0u,10u,old_body,USB_STREAM_UAC_BODY_SAMPLES)==0u,"stale current refill rejected while pending");
  Check(StreamRing_WriteVoice(0u,3u,1u,12u,newer_body,USB_STREAM_UAC_BODY_SAMPLES)==USB_STREAM_UAC_BODY_SAMPLES,"newest pending owns writes");
  Check(StreamRing_WriteVoice(0u,3u,0u,12u,newer_body,USB_STREAM_UAC_BODY_SAMPLES)==USB_STREAM_UAC_BODY_SAMPLES&&
            StreamRing_PendingFill(0u)==2u*USB_STREAM_UAC_BODY_SAMPLES,
        "confirmed pending session must accept later non-SOF BODY");
  StreamRing_DiscardPending(0u);
  Check(StreamRing_PendingFill(0u)==0u&&StreamRing_GetRel(0u,0u,&sample)==0&&sample==10,"discard pending preserves current");
  StreamRing_ArmPending(1u,20u,4u);
  Check(StreamRing_WriteVoice(1u,4u,1u,20u,new_body,USB_STREAM_UAC_BODY_SAMPLES)==USB_STREAM_UAC_BODY_SAMPLES,"credit initial frame");
  Check(StreamRing_FreeLevel(1u)==STREAM_RING_SAMPLES-USB_STREAM_UAC_BODY_SAMPLES,"credit includes both spans");
  {unsigned i;for(i=1u;i<4u;++i)Check(StreamRing_WriteVoice(1u,4u,1u,20u,new_body,USB_STREAM_UAC_BODY_SAMPLES)==USB_STREAM_UAC_BODY_SAMPLES,"fill to exact frame credit");}
  Check(StreamRing_FreeLevel(1u)==
            STREAM_RING_SAMPLES-4u*USB_STREAM_UAC_BODY_SAMPLES&&
            StreamRing_WriteBegin(1u,4u,1u,20u,USB_STREAM_UAC_BODY_SAMPLES,&write)==STREAM_RING_WRITE_ERROR,
        "exact credit prevents complete-frame overfill");
  Check(StreamRing_FutureCount()==1u&&StreamRing_SupersededCount()==1u&&
            StreamRing_StaleCount()==1u&&StreamRing_FullCount()==1u,
        "future, superseded, stale and full frames must be counted separately");
  StreamRing_ArmPending(2u,30u,5u);
  packet[0]=(int8_t)(USB_STREAM_TAG_BASE|USB_STREAM_TAG_SOF|2u);
  packet[1]=(int8_t)5u;
  packet[2]=(int8_t)0xFEu;
  packet[3]=(int8_t)0xFFu;
  Fill(packet+USB_STREAM_UAC_HEADER_BYTES,90);
  Check(StreamRing_WriteUac(packet)==1u&&
            StreamRing_LastUacSequence()==0xFFFEu&&
            StreamRing_PendingFill(2u)==USB_STREAM_UAC_BODY_SAMPLES,
        "UAC parser must separate sequence metadata from BODY");
  packet[1]=(int8_t)6u;
  packet[2]=(int8_t)0xFFu;
  packet[3]=(int8_t)0xFFu;
  packet[0]=(int8_t)(USB_STREAM_TAG_BASE|2u);
  Check(StreamRing_WriteUac(packet)==0u&&
            StreamRing_LastUacSequence()==0xFFFFu,
        "vq must acknowledge a routed frame even when its session is rejected");
  return EXIT_SUCCESS;
}
