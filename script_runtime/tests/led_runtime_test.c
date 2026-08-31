#include "script/berry_backend.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { float value[4]; unsigned calls; } LedMock;

static int read_input(void *context, uint8_t voice, FwVmChannelInput input,
                      float *value)
{
  (void)context;
  (void)voice;
  (void)input;
  *value = 60.0f;
  return 0;
}

static int set_led(void *context, uint8_t voice, float red, float green,
                   float blue, float brightness)
{
  LedMock *mock = context;
  (void)voice;
  mock->value[0] = red;
  mock->value[1] = green;
  mock->value[2] = blue;
  mock->value[3] = brightness;
  ++mock->calls;
  return 0;
}

int main(int argc, char **argv)
{
  ScriptBerryRuntime runtime;
  ScriptBerryNativeOps ops = {0};
  LedMock mock = {0};
  FILE *file;
  uint8_t *program;
  long length;

  assert(argc == 2);
  file = fopen(argv[1], "rb");
  assert(file != NULL && fseek(file, 0, SEEK_END) == 0);
  length = ftell(file);
  assert(length > 0 && fseek(file, 0, SEEK_SET) == 0);
  program = malloc((size_t)length);
  assert(program != NULL);
  assert(fread(program, 1, (size_t)length, file) == (size_t)length);
  fclose(file);

  ops.context = &mock;
  ops.read_input = read_input;
  ops.set_led = set_led;
  script_berry_init(&runtime, &ops);
  assert(script_berry_upload_begin(&runtime, 0u) == 0);
  assert(script_berry_upload_feed(&runtime, program, (size_t)length) == 0);
  assert(script_berry_upload_commit(&runtime) == 0);
  script_berry_boundary_begin(&runtime);
  {
    int result = script_berry_dispatch(&runtime, FW_VM_CHANNEL_HANDLER_NOTE_ON, 0u);
    if (result != 0)
      fprintf(stderr, "LED dispatch failed: fault=%u calls=%u callback=%u\n",
              (unsigned)script_berry_fault(&runtime, 0u), mock.calls,
              runtime.ops.set_led != NULL);
    assert(result == 0);
  }
  assert(mock.calls == 1u);
  /* MIDI 60 is halfway through the C2 (36) to C6 (84) gradient. */
  assert(fabsf(mock.value[0] - 0.5f) < 0.0001f);
  assert(fabsf(mock.value[1] - 0.5f) < 0.0001f);
  assert(fabsf(mock.value[2] - 0.0f) < 0.0001f);
  assert(fabsf(mock.value[3] - 0.8f) < 0.0001f);
  free(program);
  puts("LED native call passed");
  return 0;
}
