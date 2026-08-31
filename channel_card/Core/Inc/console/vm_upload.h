#ifndef VM_UPLOAD_H
#define VM_UPLOAD_H

#include <stdint.h>

uint8_t VmUpload_IsActive(void);
int VmUpload_Begin(uint8_t voice, uint32_t nbytes);
uint32_t VmUpload_Feed(const uint8_t *data, uint32_t size);
void VmUpload_Abort(void);

#endif
