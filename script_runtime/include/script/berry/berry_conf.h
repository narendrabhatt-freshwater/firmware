#ifndef BERRY_CONF_H
#define BERRY_CONF_H

#include <assert.h>
#include <stddef.h>

void *script_berry_malloc(size_t size);
void script_berry_free(void *ptr);
void *script_berry_realloc(void *ptr, size_t size);

#define BE_DEBUG 0
#define BE_INTGER_TYPE 0
#define BE_USE_SINGLE_FLOAT 1
#define BE_BYTES_MAX_SIZE 4096
#define BE_USE_PRECOMPILED_OBJECT 1
#define BE_DEBUG_SOURCE_FILE 0
#define BE_DEBUG_RUNTIME_INFO 2
#define BE_DEBUG_VAR_INFO 0
#define BE_USE_PERF_COUNTERS 1
#define BE_VM_OBSERVABILITY_SAMPLING 9
#define BE_STACK_TOTAL_MAX 256
#define BE_STACK_FREE_MIN 8
#define BE_STACK_START 32
#define BE_CONST_SEARCH_SIZE 32
#define BE_USE_STR_HASH_CACHE 0
#define BE_USE_FILE_SYSTEM 0
#define BE_USE_SCRIPT_COMPILER 0
#define BE_USE_BYTECODE_SAVER 0
#define BE_USE_BYTECODE_LOADER 1
#define BE_USE_SHARED_LIB 0
#define BE_USE_OVERLOAD_HASH 0
#define BE_MAX_PARSER_DEPTH 16
#define BE_USE_DEBUG_HOOK 0
#define BE_USE_DEBUG_GC 0
#define BE_USE_DEBUG_STACK 0
#define BE_USE_MEM_ALIGNED 0

#define BE_USE_STRING_MODULE 1
#define BE_USE_JSON_MODULE 0
#define BE_USE_MATH_MODULE 1
#define BE_USE_TIME_MODULE 0
#define BE_USE_OS_MODULE 0
#define BE_USE_GLOBAL_MODULE 0
#define BE_USE_SYS_MODULE 0
#define BE_USE_DEBUG_MODULE 0
#define BE_USE_GC_MODULE 0
#define BE_USE_SOLIDIFY_MODULE 0
#define BE_USE_INTROSPECT_MODULE 0
#define BE_USE_STRICT_MODULE 0

#define BE_EXPLICIT_MALLOC script_berry_malloc
#define BE_EXPLICIT_FREE script_berry_free
#define BE_EXPLICIT_REALLOC script_berry_realloc
#define be_assert(expr) assert(expr)

#endif
