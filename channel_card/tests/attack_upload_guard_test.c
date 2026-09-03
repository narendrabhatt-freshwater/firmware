#include "attack_upload.h"
#include "attack_bank.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int8_t table[ATTACK_BANK_LEN];
static uint8_t references;
static uint8_t write_active;
static uint16_t write_id;
static uint16_t committed_id;
static uint32_t committed_length;
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
void AttackBank_SetWriteActive(uint8_t active) { write_active = active; }
int8_t *AttackBank_WritePtr(uint16_t wave_id)
{
  write_id = wave_id;
  return wave_id < ATTACK_BANK_COUNT ? table : NULL;
}
int AttackBank_Commit(uint16_t wave_id, uint32_t nsamp)
{
  committed_id = wave_id;
  committed_length = nsamp;
  return 0;
}
void USB_CDC_WriteStr(const char *text)
{
  snprintf(reply, sizeof(reply), "%s", text != NULL ? text : "");
}

int main(void)
{
  const uint8_t data[2] = {0x80u, 0x7fu};
  uint16_t wave_id;

  references = 1u;
  Check(AttackUpload_BeginWavetable(0u, sizeof(data)) != 0 &&
            write_active == 0u,
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
              write_active != 0u &&
              write_id == ATTACK_BANK_WAVETABLE_FIRST + wave_id,
          "every logical wavetable must resolve to reserved card storage");
    Check(AttackUpload_Feed(data, sizeof(data)) == sizeof(data),
          "wavetable payload must be consumed");
    Check(committed_id == ATTACK_BANK_WAVETABLE_FIRST + wave_id &&
              committed_length == sizeof(data) &&
              table[0] == (int8_t)0x80 && table[1] == 0x7f,
          "reserved wavetable data and real length must commit");
  }
  Check(write_active == 0u && AttackUpload_IsActive() == 0u &&
            strcmp(reply, "ok:wavetable 7\r\n") == 0,
        "completed upload must release the bank write guard");

  puts("Attack upload guard test passed");
  return 0;
}
