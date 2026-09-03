#include "attack_bank.h"

#include <stdint.h>
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
  const int8_t head[] = {-128, -1, 0, 1, 127};
  AttackBank_Init();
  Check(ATTACK_BANK_BYTES == 512u, "attack storage must be one byte/sample");
  Check(AttackBank_Load(3u, (const uint8_t *)head, sizeof head) == 0,
        "signed-int8 attack must load");
  Check(AttackBank_GetLen(3u) == sizeof head,
        "attack byte count must equal sample count");
  Check(AttackBank_Table(3u)[0] == -128 && AttackBank_Table(3u)[4] == 127,
        "attack storage must preserve signed-int8 endpoints");
  Check(AttackBank_SampleAt(3u, 0u) == INT32_MIN,
        "-128 must expand to Q31 minimum");
  Check(AttackBank_SampleAt(3u, 4u) == 2130706432,
        "127 must expand exactly into Q31");
  Check(AttackBank_Load(3u, (const uint8_t *)head, 0u) != 0,
        "empty attack must be rejected");
  return EXIT_SUCCESS;
}
