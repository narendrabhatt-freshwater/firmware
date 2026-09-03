#include "berry.h"
#include "be_vm.h"
#include "script/script_runtime.h"
#include "freshwater/vm_source.h"

#include <errno.h>
#include <math.h>
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

typedef struct {
    uint8_t keymap[FW_SCRIPT_CHANNEL_KEY_COUNT];
    uint8_t reference_key;
    float reference_hz;
    int tuning_called;
} InitMetadata;
static InitMetadata *s_metadata;
static int init_error(bvm *vm, const char *message)
{ be_raise(vm, "value_error", message); return 0; }
static int compile_only_native(bvm *vm)
{
    if (s_metadata) return init_error(vm, "runtime function is not available in on_init");
    be_pushnil(vm);
    be_return(vm);
}
static int keymap_set_native(bvm *vm)
{
    bint input, output;
    if (!s_metadata || be_top(vm) != 2 || !be_isint(vm, 1) || !be_isint(vm, 2))
        return init_error(vm, "keymap_set requires two integer keys");
    input = be_toint(vm, 1); output = be_toint(vm, 2);
    if (input < 0 || input >= (bint)FW_SCRIPT_CHANNEL_KEY_COUNT ||
        output < 0 || output >= (bint)FW_SCRIPT_CHANNEL_KEY_COUNT)
        return init_error(vm, "keymap_set key out of range");
    s_metadata->keymap[input] = (uint8_t)output; be_pushnil(vm); be_return(vm);
}
static int keymap_fill_native(bvm *vm)
{
    bint output;
    if (!s_metadata || be_top(vm) != 1 || !be_isint(vm, 1))
        return init_error(vm, "keymap_fill requires one integer key");
    output = be_toint(vm, 1);
    if (output < 0 || output >= (bint)FW_SCRIPT_CHANNEL_KEY_COUNT)
        return init_error(vm, "keymap_fill key out of range");
    memset(s_metadata->keymap, (int)output, sizeof(s_metadata->keymap));
    be_pushnil(vm); be_return(vm);
}
static int keymap_get_native(bvm *vm)
{
    bint input;
    if (!s_metadata || be_top(vm) != 1 || !be_isint(vm, 1))
        return init_error(vm, "keymap_get requires one integer key");
    input = be_toint(vm, 1);
    if (input < 0 || input >= (bint)FW_SCRIPT_CHANNEL_KEY_COUNT)
        return init_error(vm, "keymap_get key out of range");
    be_pushint(vm, s_metadata->keymap[input]); be_return(vm);
}
static int tuning_set_native(bvm *vm)
{
    bint key; float hz;
    if (!s_metadata || be_top(vm) != 2 || !be_isint(vm, 1) || !be_isnumber(vm, 2))
        return init_error(vm, "tuning_set requires reference key and frequency");
    key = be_toint(vm, 1); hz = (float)be_toreal(vm, 2);
    if (key < 0 || key >= (bint)FW_SCRIPT_CHANNEL_KEY_COUNT || !isfinite(hz) || hz <= 0.0f)
        return init_error(vm, "invalid tuning reference");
    if (s_metadata->tuning_called)
        return init_error(vm, "tuning_set must be called exactly once");
    s_metadata->reference_key = (uint8_t)key; s_metadata->reference_hz = hz;
    s_metadata->tuning_called = 1; be_pushnil(vm); be_return(vm);
}
static int pow_native(bvm *vm)
{
    float value;
    if (be_top(vm) != 2 || !be_isnumber(vm, 1) || !be_isnumber(vm, 2))
        return init_error(vm, "pow requires two numbers");
    value = powf((float)be_toreal(vm, 1), (float)be_toreal(vm, 2));
    if (!isfinite(value)) return init_error(vm, "pow result must be finite");
    be_pushreal(vm, value); be_return(vm);
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
    return strncmp(p, "def on_init()", sizeof("def on_init()") - 1u) == 0 ||
           strncmp(p, "def on_note_on(key)",
                   sizeof("def on_note_on(key)") - 1u) == 0 ||
           strncmp(p, "def on_note_off(has_pending)",
                   sizeof("def on_note_off(has_pending)") - 1u) == 0 ||
           strncmp(p, "def on_ramp_end()", sizeof("def on_ramp_end()") - 1u) == 0 ||
           strncmp(p, "end", 3) == 0;
}

int main(int argc, char **argv)
{
    const char *input = NULL, *output = NULL;
    char temporary[1024], wrapped[1024], preprocess_error[256];
    FILE *file, *source, *wrapper;
    char *source_text = NULL, *lowered = NULL;
    size_t source_size = 0, lowered_size = 0;
    uint8_t *payload, *body, header[FW_SCRIPT_CONTAINER_HEADER_SIZE] = {0};
    InitMetadata metadata;
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
    source = fopen(input, "rb");
    if (!source || fseek(source, 0, SEEK_END) || (length = ftell(source)) < 0 ||
        fseek(source, 0, SEEK_SET)) {
        if (source) fclose(source);
        return fail("could not read source", input);
    }
    source_size = (size_t)length;
    source_text = (char *)malloc(source_size + 1u);
    if (!source_text) {
        fclose(source);
        return fail("could not read source", input);
    }
    if (fread(source_text, 1, source_size, source) != source_size) {
        fclose(source);
        free(source_text);
        return fail("could not read source", input);
    }
    if (fclose(source)) {
        free(source_text);
        return fail("could not read source", input);
    }
    source_text[source_size] = '\0';
    if (fw_vm_preprocess_channel_source(source_text, source_size, &lowered,
                                        &lowered_size, preprocess_error,
                                        sizeof(preprocess_error)) != 0) {
        free(source_text);
        return fail(preprocess_error[0] ? preprocess_error :
                    "could not preprocess named state", input);
    }
    free(source_text);
    wrapper = fopen(wrapped, "wb");
    if (!wrapper) {
        free(lowered);
        return fail("could not create wrapped source", input);
    }
    source = tmpfile();
    if (!source || fwrite(lowered, 1, lowered_size, source) != lowered_size ||
        fseek(source, 0, SEEK_SET)) {
        if (source) fclose(source);
        fclose(wrapper); free(lowered); remove(wrapped);
        return fail("could not process source", input);
    }
    free(lowered);
    while (fgets(line, sizeof(line), source)) {
        if (!source_line_allowed(line)) {
            fclose(source); fclose(wrapper); remove(wrapped);
            return fail("only ABI6 Channel handlers are allowed at top level", input);
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
    be_regfunc(vm, "discard_pending", compile_only_native);
    be_regfunc(vm, "led", compile_only_native);
    be_regfunc(vm, "keymap_set", keymap_set_native);
    be_regfunc(vm, "keymap_fill", keymap_fill_native);
    be_regfunc(vm, "keymap_get", keymap_get_native);
    be_regfunc(vm, "tuning_set", tuning_set_native);
    be_regfunc(vm, "pow", pow_native);
    compiler_int(vm, "INPUT_NOTE_ID", FW_VM_CHANNEL_INPUT_NOTE_ID);
    compiler_int(vm, "INPUT_FREQUENCY", FW_VM_CHANNEL_INPUT_FREQUENCY);
    compiler_int(vm, "INPUT_GAIN", FW_VM_CHANNEL_INPUT_GAIN);
    compiler_int(vm, "INPUT_GATE", FW_VM_CHANNEL_INPUT_GATE);
    compiler_int(vm, "INPUT_ACTIVE", FW_VM_CHANNEL_INPUT_ACTIVE);
    compiler_int(vm, "INPUT_HAS_PENDING", FW_VM_CHANNEL_INPUT_HAS_PENDING);
    compiler_int(vm, "INPUT_PENDING_FREQUENCY", FW_VM_CHANNEL_INPUT_PENDING_FREQUENCY);
    compiler_int(vm, "INPUT_PENDING_GAIN", FW_VM_CHANNEL_INPUT_PENDING_GAIN);
    compiler_int(vm, "INPUT_AMPLITUDE", FW_VM_CHANNEL_INPUT_AMPLITUDE);
    compiler_int(vm, "INPUT_KEY", FW_VM_CHANNEL_INPUT_KEY);
    compiler_int(vm, "INPUT_MAPPED_KEY", FW_VM_CHANNEL_INPUT_MAPPED_KEY);
    compiler_int(vm, "INPUT_PENDING_KEY", FW_VM_CHANNEL_INPUT_PENDING_KEY);
    compiler_int(vm, "INPUT_PENDING_MAPPED_KEY", FW_VM_CHANNEL_INPUT_PENDING_MAPPED_KEY);
    compiler_nil(vm, "on_init");
    compiler_nil(vm, "on_note_on");
    compiler_nil(vm, "on_note_off");
    compiler_nil(vm, "on_ramp_end");
    result = be_loadmode(vm, wrapped, 0);
    if (result == BE_OK) result = be_savecode(vm, temporary);
    if (result == BE_OK) result = be_pcall(vm, 0);
    memset(&metadata, 0, sizeof(metadata));
    metadata.reference_key = 60u;
    metadata.reference_hz = 261.625565f;
    for (unsigned key = 0; key < FW_SCRIPT_CHANNEL_KEY_COUNT; ++key)
        metadata.keymap[key] = (uint8_t)key;
    if (result == BE_OK) {
        bbool found = be_getglobal(vm, "on_init");
        if (found && be_isfunction(vm, -1)) {
            bvalue *value = be_indexof(vm, -1);
            if (!var_isclosure(value) ||
                ((bclosure *)var_toobj(value))->proto->argc != 0u) {
                result = BE_EXCEPTION;
            } else {
                s_metadata = &metadata; result = be_pcall(vm, 0); s_metadata = NULL;
            }
        } else {
            be_pop(vm, 1);
        }
    }
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
        length + FW_SCRIPT_CHANNEL_METADATA_SIZE > FW_SCRIPT_MAX_PAYLOAD || fseek(file, 0, SEEK_SET)) {
        fclose(file); remove(temporary);
        return fail("bytecode and metadata exceed 4096-byte limit", input);
    }
    payload = (uint8_t *)malloc((size_t)length);
    if (!payload || fread(payload, 1, (size_t)length, file) != (size_t)length) {
        fclose(file); free(payload); remove(temporary);
        return fail("could not read generated bytecode", temporary);
    }
    fclose(file);
    remove(temporary);

    body = (uint8_t *)calloc(1u, FW_SCRIPT_CHANNEL_METADATA_SIZE + (size_t)length);
    if (!body) { free(payload); return fail("could not allocate container", output); }
    body[0] = FW_SCRIPT_CHANNEL_METADATA_VERSION;
    body[FW_SCRIPT_CHANNEL_METADATA_REFERENCE_KEY_OFFSET] = metadata.reference_key;
    { uint32_t bits; memcpy(&bits, &metadata.reference_hz, sizeof(bits));
      put_u32(body + FW_SCRIPT_CHANNEL_METADATA_REFERENCE_HZ_OFFSET, bits); }
    memcpy(body + FW_SCRIPT_CHANNEL_METADATA_KEYMAP_OFFSET, metadata.keymap, sizeof(metadata.keymap));
    memcpy(body + FW_SCRIPT_CHANNEL_METADATA_SIZE, payload, (size_t)length);
    memcpy(header, "FWSC", 4);
    put_u16(header + 4, FW_SCRIPT_CONTAINER_VERSION);
    header[6] = FW_SCRIPT_RUNTIME_BERRY;
    header[7] = FW_SCRIPT_CONFIG_FLOAT32_INT32;
    put_u16(header + 8, FW_SCRIPT_CHANNEL_ABI_VERSION);
    put_u16(header + 10, FW_SCRIPT_CONTAINER_HEADER_SIZE);
    put_u32(header + 12, (uint32_t)length + FW_SCRIPT_CHANNEL_METADATA_SIZE);
    put_u32(header + 16, fw_vm_crc32(body, FW_SCRIPT_CHANNEL_METADATA_SIZE + (size_t)length));
    file = fopen(output, "wb");
    if (!file || fwrite(header, 1, sizeof(header), file) != sizeof(header) ||
        fwrite(body, 1, FW_SCRIPT_CHANNEL_METADATA_SIZE + (size_t)length, file) != FW_SCRIPT_CHANNEL_METADATA_SIZE + (size_t)length || fclose(file)) {
        if (file) fclose(file);
        free(body); free(payload);
        return fail("could not write container", output);
    }
    free(body); free(payload);
    return 0;
}
