#include "freshwater/vm.h"
uint32_t fw_vm_crc32(const void *data, size_t size) {
  const uint8_t *p=(const uint8_t *)data; uint32_t crc=UINT32_C(0xffffffff);
  while (size-- != 0u) { unsigned bit; crc^=*p++; for (bit=0u; bit<8u; ++bit)
    crc=(crc>>1)^(UINT32_C(0xedb88320)&(uint32_t)-(int32_t)(crc&1u)); }
  return ~crc;
}
