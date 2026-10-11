################################################################################
# Freshwater Channel Card — host simulator build (no STM32 toolchain needed)
################################################################################

HOST_CC ?= cc
HOST_CXX ?= c++

MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
ROOT := $(abspath $(dir $(MAKEFILE))/..)
VOICEBD := $(abspath $(ROOT)/../voice_bd)
OBJDIR := $(ROOT)/build/sim
BERRY := berry_runtime/third_party/berry
# The simulator is delivered beside the firmware binary.
SIMULATOR := $(ROOT)/bin/sim
TESTS := $(OBJDIR)/sim_test
# The interactive host programs, linked for the simulator ports. They sit
# beside the hardware builds because they look for series2.bec and the Berry
# compiler next to the executable.
APPS := $(VOICEBD)/voicebd.sim $(VOICEBD)/voicebdgui.sim
MIDI_FLAGS := $(shell pkg-config --cflags rtmidi 2>/dev/null)
MIDI_LIBS := $(shell pkg-config --libs rtmidi 2>/dev/null)
PROGRAM ?= $(VOICEBD)/series2.bec

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
.PHONY: all test apps clean

# ---- source lists ------------------------------------------------------------

# Production sources, compiled unchanged. The hardware layer (audio_i2s,
# board_stm32, usb_device, cs4304) is replaced by sim/board_sim.cpp.
CARD_SOURCES := \
    app/audio.cpp \
    app/channel.cpp \
    app/filter.cpp \
    app/oscillator.cpp \
    app/voice.cpp \
    app/samples.cpp \
    app/stream.cpp \
    app/usb.cpp \
    sim/board_sim.cpp

VM_SOURCES := berry_runtime/vm.c berry_runtime/vm_source.c \
              berry_runtime/berry_backend.c
BERRY_SOURCES := $(patsubst $(ROOT)/%,%,$(sort $(wildcard $(ROOT)/$(BERRY)/src/*.c))) \
                 berry_runtime/berry_runtime_modtab.c

CARD_OBJECTS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(CARD_SOURCES)))
VM_OBJECTS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(VM_SOURCES)))
BERRY_OBJECTS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(BERRY_SOURCES)))
CARD := $(CARD_OBJECTS) $(VM_OBJECTS) $(BERRY_OBJECTS)
SIMULATOR_OBJECTS := $(OBJDIR)/sim/main.cpp.o
TEST_OBJECTS := $(OBJDIR)/sim/sim_test.cpp.o

# The host application, built from voice_bd unchanged. sim/host supplies a
# libserialport.h that connects to the simulator's sockets instead of a tty.
HOST_OBJECTS := $(OBJDIR)/voice_bd/voicebd.o $(OBJDIR)/host/serialport.o
APP_OBJECTS := $(OBJDIR)/voice_bd/voicebdtest.o $(OBJDIR)/voice_bd/envelope_controls.o \
               $(OBJDIR)/voice_bd/voicebdgui.o

OBJECTS := $(CARD) $(SIMULATOR_OBJECTS) $(TEST_OBJECTS) $(HOST_OBJECTS) \
           $(APP_OBJECTS)

# ---- compiler settings -------------------------------------------------------

INCLUDES := -I$(ROOT)/app -I$(ROOT)/berry_runtime -I$(ROOT)/$(BERRY)/src -I$(ROOT)/sim
COMMON_FLAGS := -O2 -g -Wall
CXXFLAGS := $(COMMON_FLAGS) -std=c++17 -fno-rtti -fno-exceptions
HOST_CXXFLAGS := $(COMMON_FLAGS) -std=c++17 -Wextra -pthread -I$(ROOT)/sim/host -I$(VOICEBD)

$(VM_OBJECTS): PRIVATE_CFLAGS := -std=c11 -Wextra -Werror
$(BERRY_OBJECTS): PRIVATE_CFLAGS := -std=c11 -w

# ---- compile and link --------------------------------------------------------

$(OBJECTS): $(MAKEFILE)

$(OBJDIR)/%.c.o: $(ROOT)/%.c
	@mkdir -p "$(@D)"
	$(HOST_CC) $(INCLUDES) $(COMMON_FLAGS) $(PRIVATE_CFLAGS) -MMD -MP -MF "$(@:.o=.d)" -c "$<" -o "$@"

$(OBJDIR)/%.cpp.o: $(ROOT)/%.cpp
	@mkdir -p "$(@D)"
	$(HOST_CXX) $(INCLUDES) $(CXXFLAGS) $(PRIVATE_CXXFLAGS) -MMD -MP -MF "$(@:.o=.d)" -c "$<" -o "$@"

$(OBJDIR)/voice_bd/%.o: $(VOICEBD)/%.cpp
	@mkdir -p "$(@D)"
	$(HOST_CXX) $(HOST_CXXFLAGS) $(MIDI_FLAGS) -MMD -MP -MF "$(@:.o=.d)" -c "$<" -o "$@"

$(OBJDIR)/host/%.o: $(ROOT)/sim/host/%.cpp
	@mkdir -p "$(@D)"
	$(HOST_CXX) $(HOST_CXXFLAGS) -MMD -MP -MF "$(@:.o=.d)" -c "$<" -o "$@"

$(SIMULATOR): $(CARD) $(SIMULATOR_OBJECTS)
	@mkdir -p "$(@D)"
	$(HOST_CXX) $^ -o "$@" -lm

$(TESTS): $(CARD) $(TEST_OBJECTS)
	$(HOST_CXX) $^ -o "$@" -lm

$(VOICEBD)/voicebd.sim: $(HOST_OBJECTS) $(OBJDIR)/voice_bd/voicebdtest.o \
                        $(OBJDIR)/voice_bd/envelope_controls.o
	$(HOST_CXX) $^ -o "$@" -pthread $(MIDI_LIBS)

$(VOICEBD)/voicebdgui.sim: $(HOST_OBJECTS) $(OBJDIR)/voice_bd/voicebdgui.o
	$(HOST_CXX) $^ -o "$@" -pthread $(MIDI_LIBS)

all: $(SIMULATOR) $(TESTS)

# Needs RtMidi, like the hardware builds of these programs.
apps: $(APPS)

# Scripted protocol and playback tests in virtual time; no ports are opened.
test: $(TESTS)
	"$(TESTS)" "$(PROGRAM)"

clean:
	@rm -rf "$(OBJDIR)"
	@rm -f "$(SIMULATOR)" $(APPS)

-include $(OBJECTS:.o=.d)
