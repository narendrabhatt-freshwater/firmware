# Standalone host build: GNU Make and a C compiler.
CC ?= cc
CFLAGS ?= -O2
CPPFLAGS += -Iconfig -Ivendor/berry/src -Ishared/include
LDLIBS += -lm

EXEEXT :=
ifeq ($(OS),Windows_NT)
EXEEXT := .exe
endif

TARGET := berry$(EXEEXT)
SOURCES := berry.c $(wildcard vendor/berry/src/*.c) vendor/berry/default/be_port.c $(wildcard shared/src/*.c)
OBJECTS := $(patsubst %.c,build/make/%.o,$(SOURCES))
DEPS := $(OBJECTS:.o=.d)

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

build/make/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -MMD -MP -c $< -o $@

clean:
	$(RM) -r build/make berry berry.exe

-include $(DEPS)
