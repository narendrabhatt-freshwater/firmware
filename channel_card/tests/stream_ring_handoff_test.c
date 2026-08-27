#include "stream_ring.h"

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
  return EXIT_SUCCESS;
}
