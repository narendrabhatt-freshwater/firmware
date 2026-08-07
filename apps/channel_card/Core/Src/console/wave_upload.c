/**
 ******************************************************************************
 * @file    wave_upload.c
 * @brief   CDC binary session: wl → raw int16 LE → WaveBank_CommitLength.
 ******************************************************************************
 */

#include "wave_upload.h"
#include "wave_bank.h"
#include "usb_app.h"

#include <stdio.h>
#include <string.h>

#define WAVE_UPLOAD_CHUNK_ACK 1024u

static uint8_t s_active;
static uint8_t s_slot;
static uint32_t s_need;
static uint32_t s_got;
static uint32_t s_ack_at;
static int16_t *s_dst;

uint8_t WaveUpload_IsActive(void)
{
  return s_active;
}

void WaveUpload_Abort(void)
{
  s_active = 0u;
  s_dst = NULL;
  s_need = 0u;
  s_got = 0u;
}

int WaveUpload_Begin(uint8_t slot, uint32_t nbytes)
{
  if (slot >= WAVE_BANK_SLOTS)
  {
    return -1;
  }
  if ((nbytes & 1u) != 0u || nbytes < 2u || nbytes > WAVE_BANK_BYTES_MAX)
  {
    return -1;
  }
  if (s_active != 0u)
  {
    return -1;
  }

  s_dst = WaveBank_WritePtr(slot);
  if (s_dst == NULL)
  {
    return -1;
  }

  WaveBank_Stop(slot);
  (void)WaveBank_CommitLength(slot, 0u);

  s_slot = slot;
  s_need = nbytes;
  s_got = 0u;
  s_ack_at = WAVE_UPLOAD_CHUNK_ACK;
  s_active = 1u;
  return 0;
}

uint32_t WaveUpload_Feed(const uint8_t *buf, uint32_t len)
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

  while (s_got >= s_ack_at && s_ack_at < s_need)
  {
    snprintf(msg, sizeof msg, "ok:chunk %lu\r\n", (unsigned long)s_ack_at);
    USB_CDC_WriteStr(msg);
    s_ack_at += WAVE_UPLOAD_CHUNK_ACK;
  }

  if (s_got >= s_need)
  {
    uint32_t nsamp = s_need / 2u;
    if (WaveBank_CommitLength(s_slot, nsamp) != 0)
    {
      USB_CDC_WriteStr("err:range\r\n");
    }
    else
    {
      snprintf(msg, sizeof msg, "ok:wave %u %lu\r\n", (unsigned)s_slot,
               (unsigned long)nsamp);
      USB_CDC_WriteStr(msg);
    }
    WaveUpload_Abort();
  }

  return take;
}
