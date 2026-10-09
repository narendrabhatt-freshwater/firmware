#include "voice.h"
#include "channel.h"
#include "filter.h"
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
static int8_t pcm[1024];
static unsigned char program[8192];
static size_t program_size;
int ChannelLed_Set(float r, float g, float b, float a) {
  (void)r;
  (void)g;
  (void)b;
  (void)a;
  return 0;
}
static void tick(void) {
  NoteBank_VmBoundaryBegin();
  for (unsigned i = 0; i < 48; i++)
    (void)NoteBank_NextSample();
  NoteBank_VmBoundaryEnd();
}
static void supply(void) {
  for (unsigned v = 0; v < 8; v++) {
    unsigned session = StreamRing_CurrentSession(v);
    if (session != 255 && StreamRing_CurrentFree(v) >= 1024)
      CHECK(StreamRing_WriteBody(v, session, pcm, 1024) == 1);
    if (StreamRing_HasPending(v) && StreamRing_FreeLevel(v) >= 1024)
      CHECK(StreamRing_WriteBody(v, StreamRing_TargetSession(v), pcm, 1024) == 1);
  }
}
static void run(unsigned ms) {
  while (ms--) {
    supply();
    tick();
  }
}
static void reset(void) {
  StreamRing_Init();
  StreamRing_StatsClear();
  AttackBank_Init();
  NoteFilter_InitAll();
  NoteEnv_Init();
  NoteBank_Init();
  CHECK(AttackBank_Load(0, (uint8_t *)pcm, 512) == 0);
  for (unsigned v = 0; v < 8; v++) {
    CHECK(NoteBank_VmUploadBegin(v) == 0);
    CHECK(NoteBank_VmUploadFeed(v, program, program_size) == 0);
    CHECK(NoteBank_VmUploadCommit(v) == 0);
  }
}
static void start_all(void) {
  reset();
  for (unsigned v = 0; v < 8; v++)
    CHECK(NoteBank_NoteOnSampleSession(v, 0, 72, 100, 1) == 0);
  run(10);
  for (unsigned v = 0; v < 8; v++)
    CHECK(NoteBank_IsActive(v));
}
static void finish(void) {
  for (unsigned v = 0; v < 8; v++)
    CHECK(NoteBank_NoteOff(v) == 0);
  run(110);
  CHECK(!NoteBank_AnyActive());
  CHECK(NoteBank_HoldCount() == 0);
  for (unsigned v = 0; v < 8; v++) {
    CHECK(!StreamRing_HasPending(v));
    CHECK(NoteBank_VmFaultCount(v) == 0);
  }
  CHECK(StreamRing_DropCount() == 0 && StreamRing_FullCount() == 0);
}
int main(int argc, char **argv) {
  CHECK(argc == 2);
  FILE *f = fopen(argv[1], "rb");
  CHECK(f);
  program_size = fread(program, 1, sizeof program, f);
  fclose(f);
  CHECK(program_size);
  memset(pcm, 24, sizeof pcm);
  reset();
  CHECK(NoteBank_NoteOnSampleSession(0, 0, 72, 100, 1) == 0);
  CHECK(StreamRing_WriteBody(0, 1, pcm, 997) == 1);
  tick();
  CHECK(!NoteBank_IsActive(0));
  CHECK(NoteBank_NoteOff(0) == 0);
  tick();
  CHECK(!NoteBank_IsActive(0) && !StreamRing_HasPending(0));
  CHECK(StreamRing_WriteBody(0, 1, pcm, 100) == 0);
  CHECK(NoteBank_NoteOnSampleSession(0, 0, 60, 100, 2) == 0);
  run(10);
  CHECK(NoteBank_IsActive(0));
  finish();

  for (unsigned cancel_ms = 0; cancel_ms <= 6; cancel_ms++) {
    start_all();
    CHECK(NoteBank_NoteOnSampleSession(0, 0, 60, 100, 2) == 0);
    run(cancel_ms);
    CHECK(NoteBank_NoteOff(0) == 0);
    run(110);
    CHECK(!NoteBank_IsActive(0));
    for (unsigned v = 1; v < 8; v++)
      CHECK(NoteBank_IsActive(v));
    finish();
  }
  start_all();
  for (unsigned i = 0; i < 800; i++) {
    for (unsigned v = 0; v < 8; v++)
      CHECK(NoteBank_NoteOnSampleSession(v, 0, (i & 1) ? 48 : 72, 100, (i + 2) % 255) == 0);
    run(1);
  }
  run(10);
  for (unsigned v = 0; v < 8; v++) {
    CHECK(NoteBank_IsActive(v));
    CHECK(StreamRing_CurrentSession(v) == 801 % 255);
    CHECK(!StreamRing_HasPending(v));
  }
  finish();
  puts("PASS: idle cancellation/restart, note-off at every handover boundary with seven continuing "
       "voices, 6400 rapid replacements including session wrap, final releases, no BODY misses or "
       "VM faults");
  return 0;
}
