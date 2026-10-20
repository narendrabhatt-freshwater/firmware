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

HOST_CC ?= cc
HOST_CXX ?= c++
PORTAUDIO_PREFIX ?= $(firstword $(foreach p,/opt/local /opt/homebrew /usr/local,$(if $(wildcard $(p)/include/portaudio.h),$(p))))
PORTAUDIO_CFLAGS ?= $(if $(PORTAUDIO_PREFIX),-I$(PORTAUDIO_PREFIX)/include)
PORTAUDIO_LIBS ?= $(if $(PORTAUDIO_PREFIX),-L$(PORTAUDIO_PREFIX)/lib) -lportaudio
BERRY := berry_runtime/third_party/berry
CPPFLAGS += -I. -Iberry_runtime -I$(BERRY)/src $(PORTAUDIO_CFLAGS)
CXXFLAGS += -std=c++17 -O2 -g -Wall -Werror -pthread -MMD -MP
CFLAGS += -std=c11 -O2 -g -MMD -MP
LDLIBS += $(PORTAUDIO_LIBS) -pthread -lm
APP_SOURCES := $(wildcard *.cpp)
BERRY_SOURCES := $(wildcard $(BERRY)/src/*.c) berry_runtime/berry_runtime_modtab.c
VM_SOURCES := berry_runtime/vm.c berry_runtime/vm_source.c berry_runtime/berry_backend.c
OBJECTS := $(addprefix bin/,$(addsuffix .o,$(APP_SOURCES) $(VM_SOURCES) $(BERRY_SOURCES)))
$(addprefix bin/,$(addsuffix .o,$(BERRY_SOURCES))): CFLAGS += -w
$(addprefix bin/,$(addsuffix .o,$(VM_SOURCES))): CFLAGS += -Wall -Wextra -Werror

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
.PHONY: all clean
all: bin/simulator

bin/%.cpp.o: %.cpp makefile
	@mkdir -p "$(@D)"
	@printf 'Compiling %s\n' "$<"
	@$(HOST_CXX) $(CPPFLAGS) $(CXXFLAGS) -c "$<" -o "$@"

bin/%.c.o: %.c makefile
	$(if $(BERRY_BUILD_REPORTED),,$(info Compiling Berry runtime)$(eval BERRY_BUILD_REPORTED := 1))
	@mkdir -p "$(@D)"
	@$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

bin/simulator: $(OBJECTS)
	@printf 'Linking simulator\n'
	@$(HOST_CXX) $(LDFLAGS) $^ -o "$@" $(LDLIBS)

clean:
	@rm -rf bin

-include $(OBJECTS:.o=.d)
