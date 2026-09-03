/**
 ******************************************************************************
 * @file    attack_upload.c
 * @brief   CDC session: al → 1..ATTACK_BANK_BYTES signed int8 → Commit.
 ******************************************************************************
 */

#include "attack_upload.h"
#include "attack_bank.h"
#include "usb_app.h"

#include <stdio.h>

static uint8_t s_active;
static uint16_t s_id;
static uint32_t s_need;
static uint32_t s_got;
static int8_t *s_dst;

uint8_t AttackUpload_IsActive(void)
{
  return s_active;
}

void AttackUpload_Abort(void)
{
  s_active = 0u;
  s_dst = NULL;
  s_need = 0u;
  s_got = 0u;
}

int AttackUpload_Begin(uint16_t wave_id, uint32_t nbytes)
{
  if (wave_id >= ATTACK_BANK_COUNT || nbytes == 0u ||
      nbytes > ATTACK_BANK_BYTES)
  {
    return -1;
  }
  if (s_active != 0u)
  {
    return -1;
  }

  s_dst = AttackBank_WritePtr(wave_id);
  if (s_dst == NULL)
  {
    return -1;
  }

  {
    uint32_t i;
    uint8_t *dst_bytes = (uint8_t *)s_dst;
    for (i = 0u; i < ATTACK_BANK_BYTES; i++)
    {
      dst_bytes[i] = 0u;
    }
  }

  s_id = wave_id;
  s_need = nbytes;
  s_got = 0u;
  s_active = 1u;
  return 0;
}

uint32_t AttackUpload_Feed(const uint8_t *buf, uint32_t len)
{
  uint32_t take;
  uint32_t i;
  char msg[48];

  if (s_active == 0u || buf == NULL || s_dst == NULL)
  {
    return 0u;
  }

  take = len;
  if (take > (s_need - s_got))
  {
    take = s_need - s_got;
  }

  {
    uint8_t *dst_bytes = (uint8_t *)s_dst;
    for (i = 0u; i < take; i++)
    {
      dst_bytes[s_got + i] = buf[i];
    }
  }
  s_got += take;

  if (s_got >= s_need)
  {
    if (AttackBank_Commit(s_id, s_need) != 0)
    {
      USB_CDC_WriteStr("err:range\r\n");
    }
    else
    {
      (void)snprintf(msg, sizeof msg, "ok:attack %u\r\n", (unsigned)s_id);
      USB_CDC_WriteStr(msg);
    }
    AttackUpload_Abort();
  }

  return take;
}
