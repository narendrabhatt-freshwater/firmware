#include "berry.h"
#include "script/script_runtime.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}

static int fail(const char *message, const char *path)
{
    fprintf(stderr, "fw_scriptc: %s%s%s\n", message, path ? ": " : "", path ? path : "");
    return 1;
}

static int compile_only_native(bvm *vm)
{
    be_pushnil(vm);
    be_return(vm);
}

static void compiler_int(bvm *vm, const char *name, bint value)
{
    be_pushint(vm, value); be_setglobal(vm, name); be_pop(vm, 1);
}

static void compiler_nil(bvm *vm, const char *name)
{
    be_pushnil(vm); be_setglobal(vm, name); be_pop(vm, 1);
}

static int source_line_allowed(const char *line)
{
    const char *p = line;
    if (strstr(line, "global ") || strstr(line, "import ") ||
        strstr(line, "class ")) return 0;
    while (*p == ' ' || *p == '\t') ++p;
    if (p != line || *p == '\0' || *p == '\n' || *p == '#') return 1;
    return strncmp(p, "def on_note_on()", 16) == 0 ||
           strncmp(p, "def on_note_off()", 17) == 0 ||
           strncmp(p, "def on_ramp_end()", 17) == 0 ||
           strncmp(p, "end", 3) == 0;
}

int main(int argc, char **argv)
{
    const char *input = NULL, *output = NULL;
    char temporary[1024], wrapped[1024];
    FILE *file, *source, *wrapper;
    uint8_t *payload, header[FW_SCRIPT_CONTAINER_HEADER_SIZE] = {0};
    char line[1024];
    long length;
    bvm *vm;
    int result;

    if (argc == 4 && strcmp(argv[2], "-o") == 0) {
        input = argv[1]; output = argv[3];
    } else {
        fprintf(stderr, "usage: fw_scriptc INPUT.be -o PROGRAM.fwsc\n");
        return 2;
    }
    if (snprintf(temporary, sizeof(temporary), "%s.berry-bytecode.tmp", output) >= (int)sizeof(temporary))
        return fail("output path too long", output);
    if (snprintf(wrapped, sizeof(wrapped), "%s.berry-source.tmp", output) >= (int)sizeof(wrapped))
        return fail("output path too long", output);
    source = fopen(input, "r"); wrapper = fopen(wrapped, "w");
    if (!source || !wrapper) {
        if (source) fclose(source); if (wrapper) fclose(wrapper);
        return fail("could not create wrapped source", input);
    }
    while (fgets(line, sizeof(line), source)) {
        if (!source_line_allowed(line)) {
            fclose(source); fclose(wrapper); remove(wrapped);
            return fail("top-level script globals/modules/classes are not allowed", input);
        }
        fputs(line, wrapper);
    }
    if (ferror(source) || fclose(source) || fclose(wrapper)) {
        remove(wrapped); return fail("could not wrap source", input);
    }
    vm = be_vm_new();
    if (!vm) return fail("could not create compiler VM", NULL);
    be_regfunc(vm, "input", compile_only_native);
    be_regfunc(vm, "state_get", compile_only_native);
    be_regfunc(vm, "state_set", compile_only_native);
    be_regfunc(vm, "set_amplitude", compile_only_native);
    be_regfunc(vm, "ramp", compile_only_native);
    be_regfunc(vm, "hold", compile_only_native);
    be_regfunc(vm, "start_note", compile_only_native);
    be_regfunc(vm, "note_end", compile_only_native);
    compiler_int(vm, "INPUT_NOTE_ID", FW_VM_CHANNEL_INPUT_NOTE_ID);
    compiler_int(vm, "INPUT_FREQUENCY", FW_VM_CHANNEL_INPUT_FREQUENCY);
    compiler_int(vm, "INPUT_GAIN", FW_VM_CHANNEL_INPUT_GAIN);
    compiler_int(vm, "INPUT_GATE", FW_VM_CHANNEL_INPUT_GATE);
    compiler_int(vm, "INPUT_ACTIVE", FW_VM_CHANNEL_INPUT_ACTIVE);
    compiler_int(vm, "INPUT_HAS_PENDING", FW_VM_CHANNEL_INPUT_HAS_PENDING);
    compiler_int(vm, "INPUT_PENDING_FREQUENCY", FW_VM_CHANNEL_INPUT_PENDING_FREQUENCY);
    compiler_int(vm, "INPUT_PENDING_GAIN", FW_VM_CHANNEL_INPUT_PENDING_GAIN);
    compiler_int(vm, "INPUT_AMPLITUDE", FW_VM_CHANNEL_INPUT_AMPLITUDE);
    compiler_int(vm, "INPUT_CRASH_RELEASE", FW_VM_CHANNEL_INPUT_CRASH_RELEASE);
    compiler_nil(vm, "on_note_on");
    compiler_nil(vm, "on_note_off");
    compiler_nil(vm, "on_ramp_end");
    result = be_loadmode(vm, wrapped, 0);
    if (result == BE_OK) result = be_savecode(vm, temporary);
    if (result != BE_OK) {
        be_dumpexcept(vm);
        be_vm_delete(vm);
        remove(temporary); remove(wrapped);
        return fail("compilation failed", input);
    }
    be_vm_delete(vm); remove(wrapped);
    file = fopen(temporary, "rb");
    if (!file) return fail(strerror(errno), temporary);
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
        length > FW_SCRIPT_MAX_PAYLOAD || fseek(file, 0, SEEK_SET)) {
        fclose(file); remove(temporary);
        return fail("bytecode exceeds 4096-byte limit", input);
    }
    payload = (uint8_t *)malloc((size_t)length);
    if (!payload || fread(payload, 1, (size_t)length, file) != (size_t)length) {
        fclose(file); free(payload); remove(temporary);
        return fail("could not read generated bytecode", temporary);
    }
    fclose(file);
    remove(temporary);

    memcpy(header, "FWSC", 4);
    put_u16(header + 4, FW_SCRIPT_CONTAINER_VERSION);
    header[6] = FW_SCRIPT_RUNTIME_BERRY;
    header[7] = FW_SCRIPT_CONFIG_FLOAT32_INT32;
    put_u16(header + 8, FW_SCRIPT_CHANNEL_ABI_VERSION);
    put_u16(header + 10, FW_SCRIPT_CONTAINER_HEADER_SIZE);
    put_u32(header + 12, (uint32_t)length);
    put_u32(header + 16, fw_vm_crc32(payload, (size_t)length));
    file = fopen(output, "wb");
    if (!file || fwrite(header, 1, sizeof(header), file) != sizeof(header) ||
        fwrite(payload, 1, (size_t)length, file) != (size_t)length || fclose(file)) {
        if (file) fclose(file);
        free(payload);
        return fail("could not write container", output);
    }
    free(payload);
    return 0;
}
