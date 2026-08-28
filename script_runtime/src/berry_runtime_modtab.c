#include "berry.h"

be_extern_native_module(math);
be_extern_native_module(undefined);

BERRY_LOCAL const bntvmodule_t *const be_module_table[] = {
    &be_native_module(math),
    &be_native_module(undefined),
    NULL
};

BERRY_LOCAL bclass_array be_class_table = {NULL};
