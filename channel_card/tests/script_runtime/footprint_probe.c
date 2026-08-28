#include "script/berry_backend.h"

int main(void)
{
    ScriptRuntime runtime;
    ScriptBerryBackend berry;
    ScriptBackend backend;
    script_berry_backend_init(&berry, &backend);
    script_runtime_init(&runtime, &backend);
    return (int)(sizeof(runtime) + sizeof(berry));
}
