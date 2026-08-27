#include "stream_ring.h"
#include "usb_stream.h"

#include <stdio.h>
#include <stdlib.h>

static void Check(int ok, const char *message)
{
  if (!ok)
  {
    fprintf(stderr, "%s\n", message);
    exit(EXIT_FAILURE);
  }
}

int main(void)
{
  StreamRing_Write_t write;
  const int16_t old_body[] = {10, 11, 12, 13, 14, 15};
  const int16_t new_body[] = {20, 21, 22};
  int16_t sample;
  int16_t uac[USB_STREAM_UAC_PACKET_WORDS];
  uint32_t i;
  StreamRing_Init();

  Check(StreamRing_WriteBegin(0u, 7u, 1u, 42u, 16u, &write) ==
            STREAM_RING_WRITE_FUTURE,
        "SOF may arrive before its authoritative tagged nX");
  StreamRing_ArmReplacement(0u, 42u, 7u);
  Check(StreamRing_WriteBegin(0u, 7u, 1u, 42u, 16u, &write) ==
            STREAM_RING_WRITE_PENDING,
        "matching SOF must wait for I2S replacement origin");
  Check(StreamRing_WriteBegin(0u, 6u, 1u, 42u, 16u, &write) ==
            STREAM_RING_WRITE_STALE,
        "superseded same-wave session must be stale while pending");

  (void)StreamRing_BeginReplacement(0u, 42u, 0u);
  Check(StreamRing_WriteBegin(0u, 6u, 1u, 42u, 16u, &write) ==
            STREAM_RING_WRITE_STALE,
        "superseded same-wave session must remain stale when ready");
  Check(StreamRing_WriteBegin(0u, 7u, 1u, 42u, 16u, &write) ==
            STREAM_RING_WRITE_OK,
        "bound session must own the ready replacement");

  StreamRing_ArmReplacement(0u, 42u, 8u);
  Check(StreamRing_WriteIsCurrent(&write) == 0u,
        "new nX must retire an in-progress old reservation");
  Check(StreamRing_WriteBegin(0u, 7u, 1u, 42u, 16u, &write) ==
            STREAM_RING_WRITE_STALE,
        "late old SOF must not claim a same-wave replacement");
  Check(StreamRing_WriteBegin(0u, 8u, 1u, 42u, 16u, &write) ==
            STREAM_RING_WRITE_PENDING,
        "new matching SOF must wait rather than drop");

  (void)StreamRing_BeginReplacement(0u, 42u, 0u);
  Check(StreamRing_WriteBegin(0u, 8u, 1u, 42u, 16u, &write) ==
            STREAM_RING_WRITE_OK,
        "new matching SOF must proceed after I2S handoff");
  StreamRing_Disarm(0u);
  Check(StreamRing_WriteIsCurrent(&write) == 0u,
        "note-off must synchronously revoke producer ownership");

  StreamRing_ArmReplacement(1u, 1u, 1u);
  (void)StreamRing_BeginReplacement(1u, 1u, 0u);
  Check(StreamRing_WriteVoice(1u, 1u, 1u, 1u, old_body, 6u) == 6u,
        "initial BODY must publish");
  StreamRing_ArmReplacement(1u, 2u, 2u);
  Check(StreamRing_BeginReplacement(1u, 2u, 4u) == 4u,
        "replacement must retain exactly the requested old tail");
  Check(StreamRing_ReleaseLevel(1u) == 4u &&
            StreamRing_FillLevel(1u) == 4u,
        "one rd must initially cover only the retained old tail");
  Check(StreamRing_WriteVoice(1u, 2u, 1u, 2u, new_body, 3u) == 3u &&
            StreamRing_FillLevel(1u) == 7u,
        "new BODY must append after the old tail on the same rd/wr span");
  Check(StreamRing_GetReleaseRel(1u, 0u, &sample) == 0 && sample == 10,
        "release must begin at the shared rd");
  StreamRing_AdvanceRelease(1u, 2u);
  Check(StreamRing_ReleaseLevel(1u) == 2u &&
            StreamRing_FillLevel(1u) == 5u &&
            StreamRing_GetReleaseRel(1u, 0u, &sample) == 0 && sample == 12,
        "shared rd must advance through the old tail");
  StreamRing_AdvanceRelease(1u, 2u);
  Check(StreamRing_ReleaseLevel(1u) == 0u &&
            StreamRing_FillLevel(1u) == 3u &&
            StreamRing_GetRel(1u, 0u, &sample) == 0 && sample == 20,
        "shared rd must land on replacement BODY at release end");

  for (i = 1u; i < USB_STREAM_UAC_PACKET_WORDS; ++i)
    uac[i] = (int16_t)i;
  uac[0] = (int16_t)(USB_STREAM_TAG_BASE | (9u <<
      USB_STREAM_TAG_SESSION_SHIFT) | USB_STREAM_TAG_SOF | 2u);
  Check(StreamRing_WriteUac(uac) == 0u &&
            StreamRing_FillLevel(2u) == 0u,
        "direct UAC must reject a session before nX authority");
  StreamRing_ArmReplacement(2u, 77u, 9u);
  (void)StreamRing_BeginReplacement(2u, 77u, 0u);
  Check(StreamRing_WriteUac(uac) == 1u &&
            StreamRing_FillLevel(2u) == USB_STREAM_UAC_BODY_SAMPLES,
        "one tagged UAC packet must append 509 raw BODY samples");
  for (i = 1u; i < USB_STREAM_UAC_PACKET_WORDS; ++i)
    uac[i] = (int16_t)(i + USB_STREAM_UAC_BODY_SAMPLES);
  Check(StreamRing_WriteUac(uac) == 1u &&
            StreamRing_FillLevel(2u) == 2u * USB_STREAM_UAC_BODY_SAMPLES &&
            StreamRing_GetRel(2u, 0u, &sample) == 0 && sample == 1 &&
            StreamRing_GetRel(2u, USB_STREAM_UAC_BODY_SAMPLES, &sample) == 0 &&
            sample == (int16_t)(1u + USB_STREAM_UAC_BODY_SAMPLES),
        "repeated session/SOF tags must append without resetting the ring");
  return EXIT_SUCCESS;
}
