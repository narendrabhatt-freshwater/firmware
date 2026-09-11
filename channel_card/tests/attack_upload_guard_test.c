#include "attack_upload.h"
#include "attack_bank.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t references;
static char reply[48];

static void Check(int condition, const char *message)
{
  if (!condition)
  {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

uint8_t NoteBank_AnyBankReferences(void) { return references; }
void USB_CDC_WriteStr(const char *text)
{
  snprintf(reply, sizeof(reply), "%s", text != NULL ? text : "");
}

int main(void)
{
  const uint8_t data[2] = {0x80u, 0x7fu};
  uint16_t wave_id;

  AttackBank_Init();
  references = 1u;
  Check(AttackBank_Load(0u, data, sizeof data) == 0, "initial head");
  const int8_t *old = AttackBank_Table(0u);
  Check(AttackUpload_Begin(0u, sizeof(data)) == 0 &&
            AttackBank_WriteIsActive() == 0u,
        "sample upload must permit active voices and note admission");
  Check(AttackUpload_Begin(1u, sizeof(data)) != 0,
        "uploads must remain serialized");
  const uint8_t replacement[] = {12u, 34u};
  Check(AttackUpload_Feed(replacement, 1u) == 1u &&
            AttackBank_Table(0u) == old && old[0] == 12 && old[1] == 127 &&
            AttackBank_GetLen(0u) == 2u,
        "partial upload overwrites received bytes in place and keeps the old length");
  Check(AttackBank_NoteOn(0u, 0u, 1.0f) == 0 &&
            AttackBank_NextSample(0u) == 12 * 16777216,
        "playback may start during direct attack upload");
  AttackUpload_Abort();
  Check(AttackBank_Table(0u) == old && old[0] == 12 && old[1] == 127 &&
            AttackBank_GetLen(0u) == 2u,
        "abort retains partially overwritten data and the previous length");
  for (unsigned i = 0; i < 100; ++i) {
    Check(AttackUpload_Begin(0u, 1u) == 0 &&
              AttackUpload_Feed(replacement, 1u) == 1u,
          "repeated shorter replacement must commit");
    Check(AttackBank_Table(0u) == old && old[0] == 12 &&
              AttackBank_GetLen(0u) == 1u &&
              AttackBank_SampleAt(0u, 1u) == 0,
          "all reads must use published data and actual bounds");
  }
  Check(AttackBank_NextSample(0u) == 12 * 16777216,
        "existing playhead beyond shorter head must read safely");

  Check(AttackUpload_BeginWavetable(0u, sizeof(data)) != 0 &&
            AttackBank_WriteIsActive() == 0u,
        "upload must be rejected while bank memory is referenced");

  references = 0u;
  Check(AttackUpload_Begin(248u, sizeof(data)) != 0,
        "raw attack upload must not expose reserved physical IDs");
  Check(AttackUpload_BeginWavetable(8u, sizeof(data)) != 0 &&
            AttackUpload_BeginWavetable(0u, 1u) != 0,
        "logical wavetable index and minimum length must be validated");
  for (wave_id = 0u; wave_id < ATTACK_BANK_WAVETABLE_COUNT; wave_id++)
  {
    Check(AttackUpload_BeginWavetable((uint8_t)wave_id, sizeof(data)) == 0 &&
              AttackBank_WriteIsActive() != 0u &&
              AttackUpload_IsActive() != 0u,
          "every logical wavetable must resolve to reserved card storage");
    Check(AttackUpload_Feed(data, sizeof(data)) == sizeof(data),
          "wavetable payload must be consumed");
    Check(AttackBank_GetLen(ATTACK_BANK_WAVETABLE_FIRST + wave_id) == sizeof(data) &&
              AttackBank_Table(ATTACK_BANK_WAVETABLE_FIRST + wave_id)[0] == (int8_t)0x80 &&
              AttackBank_Table(ATTACK_BANK_WAVETABLE_FIRST + wave_id)[1] == 0x7f,
          "reserved wavetable data and real length must commit");
  }
  Check(AttackBank_WriteIsActive() == 0u && AttackUpload_IsActive() == 0u &&
            strcmp(reply, "ok:wavetable 7\r\n") == 0,
        "completed upload must release the bank write guard");

  puts("Attack upload guard test passed");
  return 0;
}
