#include "freshwater/vm_source.h"
#include "freshwater/vm.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *text;
    size_t length;
} StateName;

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} Buffer;

static int fail(char *error, size_t error_size, size_t line,
                const char *format, ...)
{
    va_list args;
    int prefix;
    if (!error || error_size == 0u) return -1;
    prefix = snprintf(error, error_size, "line %zu: ", line);
    if (prefix < 0 || (size_t)prefix >= error_size) return -1;
    va_start(args, format);
    (void)vsnprintf(error + prefix, error_size - (size_t)prefix, format, args);
    va_end(args);
    return -1;
}

static int is_identifier_start(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static int is_identifier_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static int span_equal(const char *text, size_t length, const char *literal)
{
    return strlen(literal) == length && memcmp(text, literal, length) == 0;
}

static int state_index(const StateName *states, size_t count,
                       const char *text, size_t length)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        if (states[i].length == length &&
            memcmp(states[i].text, text, length) == 0) return (int)i;
    }
    return -1;
}

static int reserved_name(const char *text, size_t length)
{
    static const char *const names[] = {
        "state", "if", "elif", "else", "while", "for", "def", "end",
        "class", "break", "continue", "return", "true", "false", "nil",
        "var", "do", "import", "as", "try", "except", "raise", "static",
        "input", "state_get", "state_set", "set_amplitude", "ramp",
        "start_note", "note_end", "discard_pending", "pitch_for_key", "osc",
        "led", "pow", "on_note_on", "on_note_off", "on_ramp_end",
        "INPUT_NOTE_ID", "INPUT_FREQUENCY", "INPUT_GAIN", "INPUT_GATE",
        "INPUT_ACTIVE", "INPUT_HAS_PENDING", "INPUT_PENDING_FREQUENCY",
        "INPUT_PENDING_GAIN", "INPUT_AMPLITUDE",
        "INPUT_KEY", "INPUT_PENDING_KEY", "INPUT_VELOCITY",
        "INPUT_PENDING_VELOCITY", "key", "velocity", "has_pending"
    };
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (span_equal(text, length, names[i])) return 1;
    }
    return 0;
}

static size_t comment_start(const char *line, size_t length)
{
    size_t i;
    char quote = 0;
    int escaped = 0;
    for (i = 0; i < length; ++i) {
        const char c = line[i];
        if (quote) {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '#') {
            return i;
        }
    }
    return length;
}

static size_t identifier_line(const char *source, size_t length,
                              const char *identifier)
{
    size_t i = 0u, line = 1u;
    char quote = 0;
    int escaped = 0;
    const size_t identifier_length = strlen(identifier);
    while (i < length) {
        const char c = source[i];
        if (c == '\n') {
            ++line;
            quote = 0;
            escaped = 0;
            ++i;
            continue;
        }
        if (quote) {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == quote) quote = 0;
            ++i;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            ++i;
            continue;
        }
        if (c == '#') {
            while (i < length && source[i] != '\n') ++i;
            continue;
        }
        if (is_identifier_start(c)) {
            size_t end = i + 1u;
            while (end < length && is_identifier_char(source[end])) ++end;
            if (end - i == identifier_length &&
                memcmp(source + i, identifier, identifier_length) == 0) return line;
            i = end;
            continue;
        }
        ++i;
    }
    return 0u;
}

static int buffer_reserve(Buffer *buffer, size_t extra)
{
    size_t needed = buffer->size + extra + 1u;
    size_t capacity = buffer->capacity ? buffer->capacity : 256u;
    char *grown;
    if (needed <= buffer->capacity) return 0;
    while (capacity < needed) {
        if (capacity > (size_t)-1 / 2u) return -1;
        capacity *= 2u;
    }
    grown = (char *)realloc(buffer->data, capacity);
    if (!grown) return -1;
    buffer->data = grown;
    buffer->capacity = capacity;
    return 0;
}

static int buffer_add(Buffer *buffer, const char *text, size_t length)
{
    if (buffer_reserve(buffer, length)) return -1;
    memcpy(buffer->data + buffer->size, text, length);
    buffer->size += length;
    buffer->data[buffer->size] = '\0';
    return 0;
}

static int buffer_add_slot_call(Buffer *buffer, const char *function, int slot)
{
    char call[32];
    int length = snprintf(call, sizeof(call), "%s(%d", function, slot);
    if (length < 0 || (size_t)length >= sizeof(call)) return -1;
    return buffer_add(buffer, call, (size_t)length);
}

static int add_with_loop_value(Buffer *output, const char *text, size_t length,
                               const char *name, int value)
{
    size_t i = 0u, copy_from = 0u;
    const size_t name_length = strlen(name);
    char number[24];
    const int number_length = snprintf(number, sizeof(number), "%d", value);
    char quote = 0;
    int escaped = 0;
    if (number_length < 0 || (size_t)number_length >= sizeof(number)) return -1;
    while (i < length) {
        const char c = text[i];
        if (quote) {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == quote) quote = 0;
            ++i;
        } else if (c == '\'' || c == '"') {
            quote = c;
            ++i;
        } else if (c == '#') {
            while (i < length && text[i] != '\n') ++i;
        } else if (is_identifier_start(c)) {
            size_t end = i + 1u;
            while (end < length && is_identifier_char(text[end])) ++end;
            if (end - i == name_length && memcmp(text + i, name, name_length) == 0) {
                if (buffer_add(output, text + copy_from, i - copy_from) ||
                    buffer_add(output, number, (size_t)number_length)) return -1;
                copy_from = end;
            }
            i = end;
        } else {
            ++i;
        }
    }
    return buffer_add(output, text + copy_from, length - copy_from);
}

/* Constant Channel loops are unrolled before Berry bytecode generation. This
 * keeps handler execution allocation-free while accepting `for i in 0..7`
 * and Berry's native `for i : 0..7` spelling. */
static int expand_constant_loops(const char *source, size_t source_size,
                                 Buffer *output, char *error,
                                 size_t error_size)
{
    size_t offset = 0u, line_number = 1u;
    while (offset < source_size) {
        size_t end = offset, content_end, indent = 0u;
        char name[64];
        int lower, upper, used = 0, matched;
        while (end < source_size && source[end] != '\n') ++end;
        if (end < source_size) ++end;
        content_end = end - offset;
        while (content_end && (source[offset + content_end - 1u] == '\n' ||
                               source[offset + content_end - 1u] == '\r')) --content_end;
        while (indent < content_end &&
               (source[offset + indent] == ' ' || source[offset + indent] == '\t')) ++indent;
        matched = sscanf(source + offset + indent,
                         "for %63[_A-Za-z0-9] in %d..%d %n",
                         name, &lower, &upper, &used);
        if (matched != 3) {
            used = 0;
            matched = sscanf(source + offset + indent,
                             "for %63[_A-Za-z0-9] : %d..%d %n",
                             name, &lower, &upper, &used);
        }
        if (matched == 3 && is_identifier_start(name[0])) {
            const char *tail = source + offset + indent + (size_t)used;
            const char *line_limit = source + offset + content_end;
            size_t body_start = end, body_end = end, scan = end;
            size_t scan_line = line_number + 1u;
            int found_end = 0;
            while (tail < line_limit && isspace((unsigned char)*tail)) ++tail;
            if (tail < line_limit && *tail != '#')
                return fail(error, error_size, line_number,
                            "constant for loop must end after its range");
            if (upper < lower || upper - lower > 31)
                return fail(error, error_size, line_number,
                            "constant for loop range must contain 1..32 values");
            while (scan < source_size) {
                size_t scan_end = scan, scan_content, scan_indent = 0u;
                size_t p, code_end;
                while (scan_end < source_size && source[scan_end] != '\n') ++scan_end;
                if (scan_end < source_size) ++scan_end;
                scan_content = scan_end - scan;
                while (scan_content && (source[scan + scan_content - 1u] == '\n' ||
                                        source[scan + scan_content - 1u] == '\r')) --scan_content;
                while (scan_indent < scan_content &&
                       (source[scan + scan_indent] == ' ' || source[scan + scan_indent] == '\t')) ++scan_indent;
                code_end = comment_start(source + scan, scan_content);
                while (code_end && isspace((unsigned char)source[scan + code_end - 1u])) --code_end;
                p = scan_indent;
                if (scan_indent == indent && code_end - p == 3u &&
                    memcmp(source + scan + p, "end", 3u) == 0) {
                    body_end = scan;
                    offset = scan_end;
                    line_number = scan_line + 1u;
                    found_end = 1;
                    break;
                }
                scan = scan_end;
                ++scan_line;
            }
            if (!found_end)
                return fail(error, error_size, line_number,
                            "constant for loop is missing end");
            {
                int value;
                for (value = lower; value <= upper; ++value) {
                    if (add_with_loop_value(output, source + body_start,
                                            body_end - body_start, name, value))
                        return fail(error, error_size, line_number, "out of memory");
                }
            }
            continue;
        }
        if (buffer_add(output, source + offset, end - offset))
            return fail(error, error_size, line_number, "out of memory");
        offset = end;
        ++line_number;
    }
    return 0;
}

static int transform_expression(Buffer *output, const char *text, size_t length,
                                const StateName *states, size_t state_count)
{
    size_t i = 0u, copy_from = 0u;
    char quote = 0;
    int escaped = 0;
    while (i < length) {
        const char c = text[i];
        if (quote) {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == quote) quote = 0;
            ++i;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            ++i;
            continue;
        }
        if (c == '#') break;
        if (is_identifier_start(c)) {
            size_t end = i + 1u;
            int slot;
            while (end < length && is_identifier_char(text[end])) ++end;
            slot = state_index(states, state_count, text + i, end - i);
            if (slot >= 0 && (i == 0u || text[i - 1u] != '.')) {
                if (buffer_add(output, text + copy_from, i - copy_from) ||
                    buffer_add_slot_call(output, "state_get", slot) ||
                    buffer_add(output, ")", 1u)) return -1;
                copy_from = end;
            }
            i = end;
            continue;
        }
        ++i;
    }
    return buffer_add(output, text + copy_from, length - copy_from);
}

static int declaration(const char *line, size_t code_end, size_t *name_start,
                       size_t *name_length, size_t *equals)
{
    size_t p = 0u, start;
    while (p < code_end && (line[p] == ' ' || line[p] == '\t')) ++p;
    if (p == 0u || p + 5u > code_end || memcmp(line + p, "state", 5u) != 0 ||
        (p + 5u < code_end && !isspace((unsigned char)line[p + 5u]))) return 0;
    p += 5u;
    while (p < code_end && isspace((unsigned char)line[p])) ++p;
    start = p;
    if (p >= code_end || !is_identifier_start(line[p])) return -1;
    while (p < code_end && is_identifier_char(line[p])) ++p;
    *name_start = start;
    *name_length = p - start;
    while (p < code_end && isspace((unsigned char)line[p])) ++p;
    if (p == code_end) {
        *equals = code_end;
        return 1;
    }
    if (line[p] != '=' || (p + 1u < code_end && line[p + 1u] == '=')) return -1;
    *equals = p;
    return 1;
}

static int assignment_operator(const char *text, size_t length,
                               size_t *operator_length, const char **binary)
{
    static const char *const operators[] = {
        "<<=", ">>=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "="
    };
    static const char *const binaries[] = {
        "<<", ">>", "+", "-", "*", "/", "%", "&", "|", "^", NULL
    };
    size_t i;
    for (i = 0; i < sizeof(operators) / sizeof(operators[0]); ++i) {
        const size_t n = strlen(operators[i]);
        if (length >= n && memcmp(text, operators[i], n) == 0 &&
            !(n == 1u && length > 1u && text[1] == '=')) {
            *operator_length = n;
            *binary = binaries[i];
            return 1;
        }
    }
    return 0;
}

static int transform_line(Buffer *output, const char *line, size_t line_length,
                          size_t line_number, const StateName *states,
                          size_t state_count, char *error, size_t error_size)
{
    size_t content_end = line_length, code_end, p = 0u, id_end, op_length = 0u;
    int slot;
    const char *binary = NULL;
    if (content_end && line[content_end - 1u] == '\n') --content_end;
    if (content_end && line[content_end - 1u] == '\r') --content_end;
    code_end = comment_start(line, content_end);
    while (code_end && isspace((unsigned char)line[code_end - 1u])) --code_end;
    while (p < code_end && (line[p] == ' ' || line[p] == '\t')) ++p;
    if (p < code_end && is_identifier_start(line[p])) {
        id_end = p + 1u;
        while (id_end < code_end && is_identifier_char(line[id_end])) ++id_end;
        slot = state_index(states, state_count, line + p, id_end - p);
        if (slot >= 0) {
            size_t op = id_end, rhs, rhs_end = code_end;
            while (op < code_end && isspace((unsigned char)line[op])) ++op;
            if (assignment_operator(line + op, code_end - op, &op_length, &binary)) {
                rhs = op + op_length;
                while (rhs < code_end && isspace((unsigned char)line[rhs])) ++rhs;
                if (rhs == code_end)
                    return fail(error, error_size, line_number,
                                "named state assignment requires a value");
                if (memchr(line + rhs, ';', rhs_end - rhs))
                    return fail(error, error_size, line_number,
                                "named state assignment must be the only statement on its line");
                if (buffer_add(output, line, p) ||
                    buffer_add_slot_call(output, "state_set", slot) ||
                    buffer_add(output, ", ", 2u)) return -2;
                if (binary) {
                    if (buffer_add_slot_call(output, "state_get", slot) ||
                        buffer_add(output, ") ", 2u) ||
                        buffer_add(output, binary, strlen(binary)) ||
                        buffer_add(output, " (", 2u)) return -2;
                }
                if (transform_expression(output, line + rhs, rhs_end - rhs,
                                         states, state_count) ||
                    (binary && buffer_add(output, ")", 1u)) ||
                    buffer_add(output, ")", 1u) ||
                    buffer_add(output, line + code_end, line_length - code_end)) return -2;
                return 0;
            }
        }
    }
    if (transform_expression(output, line, line_length, states, state_count)) return -2;
    return 0;
}

static int preprocess_channel_source(const char *source, size_t source_size,
                                     char **output, size_t *output_size,
                                     char *error, size_t error_size)
{
    StateName states[FW_SCRIPT_CHANNEL_STATE_VALUES];
    size_t state_count = 0u, offset = 0u, line_number = 1u;
    Buffer result = {0};
    if (!source || !output || !output_size) return -1;
    *output = NULL;
    *output_size = 0u;
    if (error && error_size) error[0] = '\0';

    while (offset < source_size) {
        size_t end = offset, content_end, code_end, name_start, name_length, equals;
        int kind;
        while (end < source_size && source[end] != '\n') ++end;
        content_end = end;
        if (content_end > offset && source[content_end - 1u] == '\r') --content_end;
        code_end = comment_start(source + offset, content_end - offset);
        while (code_end && isspace((unsigned char)source[offset + code_end - 1u])) --code_end;
        kind = declaration(source + offset, code_end, &name_start, &name_length, &equals);
        if (kind < 0)
            return fail(error, error_size, line_number, "invalid named state declaration");
        if (kind > 0) {
            if (reserved_name(source + offset + name_start, name_length))
                return fail(error, error_size, line_number,
                            "reserved name cannot be used for persistent state");
            if (state_index(states, state_count, source + offset + name_start,
                            name_length) >= 0)
                return fail(error, error_size, line_number,
                            "persistent state '%.*s' is declared more than once",
                            (int)name_length, source + offset + name_start);
            if (state_count >= FW_SCRIPT_CHANNEL_STATE_VALUES)
                return fail(error, error_size, line_number,
                            "too many persistent state variables (maximum is %u)",
                            (unsigned)FW_SCRIPT_CHANNEL_STATE_VALUES);
            states[state_count].text = source + offset + name_start;
            states[state_count].length = name_length;
            ++state_count;
        }
        offset = end < source_size ? end + 1u : end;
        ++line_number;
    }

    if (state_count) {
        size_t raw_line = identifier_line(source, source_size, "state_get");
        if (!raw_line) raw_line = identifier_line(source, source_size, "state_set");
        if (raw_line)
            return fail(error, error_size, raw_line,
                        "named and numeric persistent state access cannot be mixed");
    }

    offset = 0u;
    line_number = 1u;
    while (offset < source_size) {
        size_t end = offset, line_length, content_end, code_end;
        size_t name_start, name_length, equals;
        int kind;
        while (end < source_size && source[end] != '\n') ++end;
        if (end < source_size) ++end;
        line_length = end - offset;
        content_end = line_length;
        if (content_end && source[offset + content_end - 1u] == '\n') --content_end;
        if (content_end && source[offset + content_end - 1u] == '\r') --content_end;
        code_end = comment_start(source + offset, content_end);
        while (code_end && isspace((unsigned char)source[offset + code_end - 1u])) --code_end;
        kind = declaration(source + offset, code_end, &name_start, &name_length, &equals);
        if (kind > 0) {
            const int slot = state_index(states, state_count,
                                         source + offset + name_start, name_length);
            size_t indent = 0u;
            while (indent < code_end &&
                   (source[offset + indent] == ' ' || source[offset + indent] == '\t')) ++indent;
            if (equals < code_end) {
                size_t rhs = equals + 1u;
                while (rhs < code_end && isspace((unsigned char)source[offset + rhs])) ++rhs;
                if (rhs == code_end) {
                    free(result.data);
                    return fail(error, error_size, line_number,
                                "named state initializer requires a value");
                }
                if (buffer_add(&result, source + offset, indent) ||
                    buffer_add_slot_call(&result, "state_set", slot) ||
                    buffer_add(&result, ", ", 2u) ||
                    transform_expression(&result, source + offset + rhs, code_end - rhs,
                                         states, state_count) ||
                    buffer_add(&result, ")", 1u) ||
                    buffer_add(&result, source + offset + code_end,
                               line_length - code_end)) {
                    free(result.data);
                    return fail(error, error_size, line_number, "out of memory");
                }
            } else {
                if (buffer_add(&result, source + offset, indent) ||
                    buffer_add(&result, source + offset + code_end,
                               line_length - code_end)) {
                    free(result.data);
                    return fail(error, error_size, line_number, "out of memory");
                }
            }
        } else {
            const int transformed = transform_line(&result, source + offset, line_length,
                                                   line_number, states, state_count,
                                                   error, error_size);
            if (transformed) {
                free(result.data);
                if (transformed == -2)
                    return fail(error, error_size, line_number, "out of memory");
                return -1;
            }
        }
        offset = end;
        ++line_number;
    }
    if (!result.data && buffer_reserve(&result, 0u))
        return fail(error, error_size, 1u, "out of memory");
    result.data[result.size] = '\0';
    *output = result.data;
    *output_size = result.size;
    return 0;
}

int fw_vm_preprocess_channel_source(const char *source, size_t source_size,
                                    char **output, size_t *output_size,
                                    char *error, size_t error_size)
{
    Buffer expanded = {0};
    int result;
    if (!source || !output || !output_size) return -1;
    if (expand_constant_loops(source, source_size, &expanded, error, error_size)) {
        free(expanded.data);
        return -1;
    }
    result = preprocess_channel_source(expanded.data, expanded.size, output,
                                       output_size, error, error_size);
    free(expanded.data);
    return result;
}
