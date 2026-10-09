#include "stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                              \
      exit(1);                                                                                     \
    }                                                                                              \
  } while (0)
static int8_t data[1024];
static void put(unsigned session, unsigned count, int8_t value) {
  memset(data, value, sizeof data);
  CHECK(StreamRing_WriteBody(0, session, data, count) == 1);
}
static void current_is(unsigned count, int8_t value) {
  CHECK(StreamRing_CurrentFill(0) == count);
  for (unsigned i = 0; i < count; i++) {
    int8_t sample;
    CHECK(StreamRing_GetRel(0, i, &sample) == 0);
    CHECK(sample == value);
  }
  int8_t sample;
  CHECK(StreamRing_GetRel(0, count, &sample) == -1);
}
static StreamRing_Write_t reserve(unsigned session, unsigned count, int8_t value) {
  StreamRing_Write_t w;
  CHECK(StreamRing_WriteBegin(0, session, 0, 0, count, &w) == STREAM_RING_WRITE_OK);
  while (w.written < w.nsamp) {
    unsigned n;
    int8_t *p = StreamRing_WriteSpan(&w, &n);
    CHECK(p && n);
    memset(p, value, n);
    CHECK(StreamRing_WriteAdvance(&w, n) == 0);
  }
  return w;
}
static void start(void) {
  StreamRing_ResetAll();
  StreamRing_StatsClear();
  StreamRing_ArmPending(0, 0, 1);
  put(1, 100, 11);
  CHECK(StreamRing_StartNote(0) == 0);
}
int main(void) {
  StreamRing_Init();
  start();
  StreamRing_ArmPending(0, 0, 2);
  put(1, 200, 11);
  put(2, 400, 22);
  current_is(300, 11);
  CHECK(StreamRing_PendingFill(0) == 400);
  CHECK(StreamRing_CurrentFree(0) == 3360 && StreamRing_FreeLevel(0) == 3360);
  CHECK(StreamRing_StartNote(0) == 0);
  current_is(400, 22);
  CHECK(StreamRing_WriteBody(0, 1, data, 1) == 0);
  current_is(400, 22);

  start();
  StreamRing_ArmPending(0, 0, 2);
  put(2, 100, 22);
  StreamRing_Write_t old = reserve(1, 50, 11), next = reserve(2, 50, 22);
  CHECK(StreamRing_StartNote(0) == 0);
  CHECK(!StreamRing_WriteIsCurrent(&old));
  CHECK(StreamRing_WriteCommit(&old) == 0);
  CHECK(StreamRing_WriteIsCurrent(&next));
  CHECK(StreamRing_WriteCommit(&next) == 50);
  current_is(150, 22);

  start();
  old = reserve(1, 50, 11);
  StreamRing_ArmPending(0, 0, 2);
  next = reserve(2, 50, 22);
  StreamRing_DiscardPending(0);
  CHECK(StreamRing_WriteCommit(&old) == 50);
  CHECK(StreamRing_WriteCommit(&next) == 0);
  current_is(150, 11);

  start();
  StreamRing_ArmPending(0, 0, 2);
  next = reserve(2, 50, 22);
  StreamRing_EndCurrent(0);
  CHECK(StreamRing_WriteCommit(&next) == 50);
  CHECK(StreamRing_StartNote(0) == 0);
  current_is(50, 22);

  start();
  old = reserve(1, 50, 11);
  StreamRing_ArmPending(0, 0, 2);
  next = reserve(2, 50, 22);
  StreamRing_ArmPending(0, 0, 3);
  CHECK(StreamRing_WriteCommit(&next) == 0);
  CHECK(StreamRing_WriteCommit(&old) == 50);
  put(3, 100, 33);
  current_is(150, 11);
  CHECK(StreamRing_StartNote(0) == 0);
  current_is(100, 33);

  start();
  StreamRing_Advance(0, 100);
  for (unsigned s = 2; s < 800; s++) {
    unsigned session = s % 255;
    StreamRing_ArmPending(0, 0, session);
    int8_t value = (int8_t)(s % 100 + 1);
    for (unsigned j = 0; j < 3; j++)
      put(session, 1024, value);
    put(session, 1008, value);
    CHECK(StreamRing_FreeLevel(0) == 0);
    CHECK(StreamRing_StartNote(0) == 0);
    current_is(4080, value);
    StreamRing_Advance(0, 3000);
    unsigned free = StreamRing_FreeLevel(0);
    unsigned expected = 1080 + free;
    while (free) {
      unsigned n = free > 1024 ? 1024 : free;
      put(session, n, value);
      free -= n;
    }
    current_is(expected, value);
    StreamRing_Advance(0, 4080);
  }
  start();
  StreamRing_ArmPending(0, 0, 2);
  put(2, 1024, 22);
  while (StreamRing_FreeLevel(0) > 0) {
    unsigned n = StreamRing_FreeLevel(0);
    put(1, n > 1024 ? 1024 : n, 11);
  }
  CHECK(StreamRing_CurrentFill(0) + StreamRing_PendingFill(0) <= 4080);
  CHECK(StreamRing_WriteBegin(0, 1, 0, 0, 1, &old) == STREAM_RING_WRITE_ERROR);
  CHECK(StreamRing_WriteBegin(0, 2, 0, 0, 1, &next) == STREAM_RING_WRITE_ERROR);
  CHECK(StreamRing_StartNote(0) == 0);
  current_is(1024, 22);
  CHECK(StreamRing_FreeLevel(0) == 3056);
  StreamRing_ResetAll();
  CHECK(StreamRing_CurrentFill(0) == 0 && !StreamRing_HasPending(0));
  start();
  StreamRing_Advance(0, 100);
  int8_t expected[4080], readback[4080];
  unsigned total = 0;
  while (StreamRing_FreeLevel(0) >= 32u) {
    unsigned n = 1u + (total % 31u);
    for (unsigned j = 0; j < n; j++)
      data[j] = (int8_t)(total + j);
    CHECK(StreamRing_WriteBody(0, 1, data, n) == 1);
    memcpy(expected + total, data, n);
    total += n;
  }
  CHECK(StreamRing_Read(0, 0, readback, total) == 0);
  CHECK(memcmp(expected, readback, total) == 0);
  CHECK(StreamRing_Read(0, total, readback, 1) == -1);
  unsigned consumed = 0;
  while (consumed < total) {
    unsigned n = 1u + (consumed % 37u);
    if (n > total - consumed)
      n = total - consumed;
    CHECK(StreamRing_Read(0, 0, readback, n) == 0);
    CHECK(memcmp(expected + consumed, readback, n) == 0);
    CHECK(StreamRing_Consume(0, readback, n) == n);
    CHECK(memcmp(expected + consumed, readback, n) == 0);
    consumed += n;
    CHECK(StreamRing_CurrentFill(0) == total - consumed);
  }
  CHECK(StreamRing_Consume(0, readback, 2) == 0);
  CHECK(StreamRing_FreeLevel(0) == 4080);
  puts("PASS: simultaneous sessions, promotion/cancellation reservations, supersession, session "
       "wrap, physical wrap, shared capacity, reset");
  return 0;
}
