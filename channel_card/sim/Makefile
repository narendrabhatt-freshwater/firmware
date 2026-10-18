#                                    __
#                                ___  \  \
#                           ___  \  \  \  \    _______
#                      ___  \  \  \  \  \__\__/_____  \
#                      \  \  \  \  \__\_______      \__\____
#                       \  \  \  \     ___ \  \________  \__/
#                        \  \  \__\___/_  \ \___/   \  \___
#                         \  \    ____  \  \________ \ ___/
#                          \__\__/ \  \  \____/  \  \
#                                   \  \     ___  \  \___
#                                    \__\___/ \  \ \____/
#                                            \  \
#                                               \  \___
#                  __                 _          \ ___/  _
#                 / _|_ __  ___  ___ | |____      ____ _| |_  ___  _ __
#                | |_| '_ |/ _ \/ __|| '_ \ \ /\ / / _` | __|/ _ \| '__|
#                |  _| |  |  __/\__ \| | | \ V  V / (_| | |_|  __/| |
#                |_| |_|   \___||___/|_| |_|\_/\_/ \__,_|\__|\___||_|
#
#             (C) 2 0 2 6   F r e s h w a t e r   I n s t r u m e n t s

# ---- sources and settings ---------------------------------------------------

HOST_CC ?= cc
HOST_CXX ?= c++
MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
ROOT := $(abspath $(dir $(MAKEFILE))/..)
OBJDIR := $(ROOT)/build/sim
SIMULATOR := $(ROOT)/bin/sim
BERRY := berry_runtime/third_party/berry

BERRY_SOURCES := $(patsubst $(ROOT)/%,%,$(sort $(wildcard $(ROOT)/$(BERRY)/src/*.c))) \
                 berry_runtime/berry_runtime_modtab.c
include $(ROOT)/app/sources.mk

APP_SOURCES := $(COMMON_APP_SOURCES)
SOURCES := $(APP_SOURCES) \
           berry_runtime/vm.c berry_runtime/berry_backend.c \
           $(BERRY_SOURCES) sim/main.cpp sim/serialdevices.cpp sim/clock.cpp \
           sim/audio_device.cpp
OBJECTS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(SOURCES)))

PORTAUDIO_PREFIX ?= $(firstword $(foreach p,/opt/local /opt/homebrew /usr/local,$(if $(wildcard $(p)/include/portaudio.h),$(p))))
PORTAUDIO_CFLAGS ?= $(if $(PORTAUDIO_PREFIX),-I$(PORTAUDIO_PREFIX)/include)
PORTAUDIO_LIBS ?= $(if $(PORTAUDIO_PREFIX),-L$(PORTAUDIO_PREFIX)/lib) -lportaudio
INCLUDES := -I$(ROOT)/app -I$(ROOT)/berry_runtime -I$(ROOT)/$(BERRY)/src -I$(ROOT)/sim $(PORTAUDIO_CFLAGS)
COMMON_FLAGS := -O2 -g -Wall -MMD -MP -pthread
CFLAGS := -std=c11 -Wextra -Werror
CXXFLAGS := -std=c++17 -fno-rtti -fno-exceptions -fno-threadsafe-statics
LDLIBS := -lm -pthread $(PORTAUDIO_LIBS)
$(addprefix $(OBJDIR)/,$(addsuffix .o,$(BERRY_SOURCES))): CFLAGS := -std=c11 -w

# ---- build and clean --------------------------------------------------------

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
.PHONY: all clean
all: $(SIMULATOR)

$(OBJECTS): $(MAKEFILE) $(ROOT)/app/sources.mk

$(OBJDIR)/%.c.o: $(ROOT)/%.c
	@mkdir -p "$(@D)"
	$(HOST_CC) $(INCLUDES) $(COMMON_FLAGS) $(CFLAGS) -c "$<" -o "$@"

$(OBJDIR)/%.cpp.o: $(ROOT)/%.cpp
	@mkdir -p "$(@D)"
	$(HOST_CXX) $(INCLUDES) $(COMMON_FLAGS) $(CXXFLAGS) -c "$<" -o "$@"

$(SIMULATOR): $(OBJECTS)
	@mkdir -p "$(@D)"
	$(HOST_CXX) $^ -o "$@" $(LDLIBS)

clean:
	@rm -rf "$(OBJDIR)"
	@rm -f "$(SIMULATOR)"

-include $(OBJECTS:.o=.d)
