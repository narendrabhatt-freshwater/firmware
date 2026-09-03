#ifndef FRESHWATER_VM_H
#define FRESHWATER_VM_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FW_SCRIPT_CONTAINER_VERSION 1u
#define FW_SCRIPT_RUNTIME_BERRY 1u
#define FW_SCRIPT_CONFIG_FLOAT32_INT32 0x03u
#define FW_SCRIPT_CHANNEL_ABI_VERSION 7u
#define FW_SCRIPT_CONTAINER_HEADER_SIZE 20u
#define FW_SCRIPT_MAX_PAYLOAD 4096u
#define FW_SCRIPT_CHANNEL_VOICE_COUNT 8u
#define FW_SCRIPT_CHANNEL_HANDLER_COUNT 3u
#define FW_SCRIPT_CHANNEL_STATE_VALUES 16u
#define FW_SCRIPT_CHANNEL_KEY_COUNT 128u
#define FW_SCRIPT_CHANNEL_METADATA_VERSION 1u
#define FW_SCRIPT_CHANNEL_METADATA_SIZE 136u
#define FW_SCRIPT_CHANNEL_METADATA_REFERENCE_KEY_OFFSET 1u
#define FW_SCRIPT_CHANNEL_METADATA_REFERENCE_HZ_OFFSET 4u
#define FW_SCRIPT_CHANNEL_METADATA_KEYMAP_OFFSET 8u
#define FW_SCRIPT_TARGET_CHANNEL UINT32_C(0x4e414843)
typedef enum { FW_VM_FAULT_NONE=0, FW_VM_FAULT_UPLOAD, FW_VM_FAULT_BAD_CONTAINER,
  FW_VM_FAULT_WRONG_TARGET, FW_VM_FAULT_BAD_PROGRAM, FW_VM_FAULT_BAD_HANDLER,
  FW_VM_FAULT_BUDGET, FW_VM_FAULT_NONFINITE, FW_VM_FAULT_BAD_HOST_ARGUMENT,
  FW_VM_FAULT_HOST_CALL, FW_VM_FAULT_NO_PROGRAM, FW_VM_FAULT_EXCEPTION,
  FW_VM_FAULT_ALLOCATION, FW_VM_FAULT_GC, FW_VM_FAULT_SHARED_VM } FwVmFault;
typedef struct { uint32_t dispatches, instructions_total, instructions_max,
  boundary_cycles_max, faults; } FwVmMetrics;
typedef struct { uint32_t arena_size, arena_current, arena_peak,
  arena_largest_free, load_allocations, handler_allocations, load_gc,
  handler_gc; uint8_t shared_vm_valid; } FwVmMemoryMetrics;
uint32_t fw_vm_crc32(const void *data, size_t size);
float fw_vm_channel_standard_hz(uint8_t key);
#ifdef __cplusplus
}
#endif
#endif
