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
TEST_ROOT := $(abspath $(dir $(MAKEFILE)))
ROOT ?= $(abspath $(TEST_ROOT)/../../../channel_card)
OBJDIR ?= /tmp/channel-host-tests
OUTPUT ?= $(OBJDIR)/test
BERRY := berry_runtime/third_party/berry
# run.py supplies the application/backend/harness source selection.
BERRY_SOURCES := $(patsubst $(ROOT)/%,%,$(sort $(wildcard $(ROOT)/$(BERRY)/src/*.c))) \
                 berry_runtime/berry_runtime_modtab.c
OBJECTS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(SOURCES)))

INCLUDES := -I$(ROOT)/app -I$(ROOT)/berry_runtime -I$(ROOT)/$(BERRY)/src -I$(ROOT)/sim
COMMON_FLAGS := -O2 -g -Wall -MMD -MP
CFLAGS := -std=c11 -Wextra -Werror
CXXFLAGS := -std=c++17 -fno-rtti -fno-exceptions -fno-threadsafe-statics
$(addprefix $(OBJDIR)/,$(addsuffix .o,$(BERRY_SOURCES))): CFLAGS := -std=c11 -w

# ---- build and clean --------------------------------------------------------

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
.PHONY: all clean
all: $(OUTPUT)

$(OBJECTS): $(MAKEFILE)

$(OBJDIR)/%.c.o: $(ROOT)/%.c
	@mkdir -p "$(@D)"
	$(HOST_CC) $(INCLUDES) $(COMMON_FLAGS) $(CFLAGS) -c "$<" -o "$@"

$(OBJDIR)/tests/%.cpp.o: $(TEST_ROOT)/%.cpp
	@mkdir -p "$(@D)"
	$(HOST_CXX) $(INCLUDES) $(COMMON_FLAGS) $(CXXFLAGS) -c "$<" -o "$@"

$(OBJDIR)/%.cpp.o: $(ROOT)/%.cpp
	@mkdir -p "$(@D)"
	$(HOST_CXX) $(INCLUDES) $(COMMON_FLAGS) $(CXXFLAGS) -c "$<" -o "$@"

$(OUTPUT): $(OBJECTS)
	@mkdir -p "$(@D)"
	$(HOST_CXX) $^ -o "$@" -lm

clean:
	@rm -rf "$(OBJDIR)"
	@rm -f "$(OUTPUT)"

-include $(OBJECTS:.o=.d)
