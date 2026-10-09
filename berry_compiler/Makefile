# Standalone host build: GNU Make and a C compiler.

CC       ?= cc
TARGET   ?= native
ZIG      ?= zig
CFLAGS   ?= -O2

CPPFLAGS += -Iconfig -Ivendor/berry/src -Ishared/include/freshwater -Ishared/include/script
LDLIBS   += -lm

EXEEXT :=
ifeq ($(OS),Windows_NT)
    EXEEXT := .exe
endif

ifeq ($(TARGET),native)
    BUILD_CC := $(CC)
    OUTPUT   := berry$(EXEEXT)
else ifeq ($(TARGET),linux-arm64)
    BUILD_CC       := $(ZIG) cc -target aarch64-linux-musl
    TARGET_LDFLAGS := -static
    OUTPUT         := berry.linux-arm64
else
    $(error Unsupported TARGET '$(TARGET)'; use native or linux-arm64)
endif

BUILD_DIR := build/make/$(TARGET)

SOURCES := \
    berry.c \
    $(wildcard vendor/berry/src/*.c) \
    vendor/berry/default/be_port.c \
    $(wildcard shared/src/*.c)

OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SOURCES))
DEPS    := $(OBJECTS:.o=.d)

.PHONY: all linux clean

all: $(OUTPUT)

linux:
	$(MAKE) TARGET=linux-arm64 all

$(OUTPUT): $(OBJECTS)
	$(BUILD_CC) $(LDFLAGS) $(TARGET_LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(BUILD_DIR)/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(BUILD_CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -MMD -MP -c $< -o $@

clean:
	$(RM) -r build/make berry berry.exe berry.linux-arm64

-include $(DEPS)
