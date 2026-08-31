#include "vm_upload.h"

#include "freshwater/vm.h"
#include "freshwater/vm_channel.h"
#include "note_bank.h"
#include "usb_app.h"

#include <stdio.h>

static uint8_t s_active;
static uint8_t s_voice;
static uint32_t s_expected;
static uint32_t s_received;

uint8_t VmUpload_IsActive(void) { return s_active; }

void VmUpload_Abort(void)
{
  if (s_active != 0u) NoteBank_VmUploadAbort(s_voice);
  s_active = 0u;
  s_expected = 0u;
  s_received = 0u;
}

int VmUpload_Begin(uint8_t voice, uint32_t nbytes)
{
  if (voice >= FW_VM_CHANNEL_VOICE_COUNT || s_active != 0u ||
      nbytes < FW_SCRIPT_CONTAINER_HEADER_SIZE ||
      nbytes > FW_SCRIPT_CONTAINER_HEADER_SIZE + FW_SCRIPT_MAX_PAYLOAD ||
      NoteBank_VmUploadBegin(voice) != 0) return -1;
  s_voice = voice;
  s_expected = nbytes;
  s_received = 0u;
  s_active = 1u;
  return 0;
}

uint32_t VmUpload_Feed(const uint8_t *data, uint32_t size)
{
  uint32_t take;
  if (s_active == 0u || data == NULL) return 0u;
  take = size;
  if (take > s_expected - s_received) take = s_expected - s_received;
  if (NoteBank_VmUploadFeed(s_voice, data, take) != 0) {
    USB_CDC_WriteStr("err:vm-container\r\n");
    VmUpload_Abort();
    return take;
  }
  s_received += take;
  if (s_received == s_expected) {
    char reply[64];
    if (NoteBank_VmUploadCommit(s_voice) != 0) {
      (void)snprintf(reply, sizeof(reply), "err:vm %u %u\r\n",
                     (unsigned)s_voice, (unsigned)NoteBank_VmFault(s_voice));
    } else {
      (void)snprintf(reply, sizeof(reply), "ok:vm %u %lu %u\r\n",
                     (unsigned)s_voice, (unsigned long)FW_VM_TARGET_CHANNEL,
                     (unsigned)FW_VM_TARGET_CHANNEL_VERSION);
    }
    USB_CDC_WriteStr(reply);
    s_active = 0u;
    s_expected = 0u;
    s_received = 0u;
  }
  return take;
}
