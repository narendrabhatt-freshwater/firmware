#ifndef FRESHWATER_VM_SOURCE_H
#define FRESHWATER_VM_SOURCE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Lower Channel-script `state name[ = expression]` declarations and named
 * state references to state_get/state_set calls. The returned buffer is
 * allocated with malloc and must be released with free.
 */
int fw_vm_preprocess_channel_source(const char *source, size_t source_size,
                                    char **output, size_t *output_size,
                                    char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
