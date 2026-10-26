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

# ---- compiler and library selection ------------------------------------------

HOST_CXX               ?= c++
HOST_ARCH              := $(shell uname -m)
OBJDIR                  = object.$(HOST_ARCH)
ARMOBJDIR               = object.arm
BINDIR                  = bin
CROSS_COMPILE          ?= arm-none-eabi-
CC                      = $(CROSS_COMPILE)gcc
CXX                     = $(CROSS_COMPILE)g++
OBJCOPY                 = $(CROSS_COMPILE)objcopy
SIZE                    = $(CROSS_COMPILE)size
PORTAUDIO_PREFIX       ?= $(firstword $(foreach p,/opt/local /opt/homebrew /usr/local,$(if $(wildcard $(p)/include/portaudio.h),$(p))))
PORTAUDIO_CFLAGS       ?= $(if $(PORTAUDIO_PREFIX),-I$(PORTAUDIO_PREFIX)/include)
PORTAUDIO_LIBS         ?= $(if $(PORTAUDIO_PREFIX),-L$(PORTAUDIO_PREFIX)/lib) -lportaudio
RS485_BAUD             ?= 921600

# ---- compiler and linker settings --------------------------------------------

STD                    ?= c++17
OPTIMIZATION           ?= -O2
ARM_OPTIMIZATION       ?= -O3
DEBUG_FLAGS            ?= -g
WARNING_FLAGS          ?=
CPPFLAGS               += -I.
CXXFLAGS               += -std=$(STD) $(OPTIMIZATION) $(DEBUG_FLAGS) $(WARNING_FLAGS) -MMD -MP
LDLIBS                 += -lm
CPU_FLAGS               = -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard
ARM_CPPFLAGS            = -DSTM32H725xx -DRS485_BAUD=$(RS485_BAUD)u
ARM_CXXFLAGS            = $(CPU_FLAGS) -ffunction-sections -fdata-sections \
                          -fno-exceptions -fno-rtti -fno-threadsafe-statics
ARM_LDFLAGS             = $(CPU_FLAGS) -TSTM32H725xG_flash.ld \
                          --specs=nano.specs -Wl,--gc-sections

# ---- objects -----------------------------------------------------------------

OBJECTS                 = $(OBJDIR)/main.o $(OBJDIR)/channel.o $(OBJDIR)/filter.o
ARM_OBJECTS             = $(ARMOBJDIR)/main_firmware.o $(ARMOBJDIR)/channel.o \
                          $(ARMOBJDIR)/filter.o \
                          $(ARMOBJDIR)/clock.o $(ARMOBJDIR)/dac.o \
                          $(ARMOBJDIR)/uart_rs485.o $(ARMOBJDIR)/startup.o

# ---- targets -----------------------------------------------------------------

.DEFAULT_GOAL          := all
.DELETE_ON_ERROR:
.PHONY                 : all simulator firmware clean

all                    : simulator
simulator              : $(BINDIR)/simulator
firmware               : $(BINDIR)/channel.bin

# ---- simulator build rules ---------------------------------------------------

$(OBJDIR)/%.o: %.cpp makefile
	@mkdir -p "$(@D)"
	@printf 'Compiling %s\n' "$<"
	@$(HOST_CXX) $(CPPFLAGS) $(PORTAUDIO_CFLAGS) $(CXXFLAGS) -pthread -c "$<" -o "$@"

$(BINDIR)/simulator: $(OBJECTS) makefile
	@mkdir -p "$(@D)"
	@printf 'Linking simulator\n'
	@$(HOST_CXX) $(LDFLAGS) $(OBJECTS) -o "$@" $(PORTAUDIO_LIBS) -pthread $(LDLIBS)

# ---- firmware build rules ----------------------------------------------------

$(ARM_OBJECTS): OPTIMIZATION = $(ARM_OPTIMIZATION)

$(ARMOBJDIR)/%.o: %.cpp makefile
	@mkdir -p "$(@D)"
	@printf 'Compiling %s\n' "$<"
	@$(CXX) $(CPPFLAGS) $(ARM_CPPFLAGS) $(CXXFLAGS) $(ARM_CXXFLAGS) -c "$<" -o "$@"

$(ARMOBJDIR)/startup.o: startup_stm32h725xx.s makefile
	@mkdir -p "$(@D)"
	@printf 'Assembling %s\n' "$<"
	@$(CC) $(CPU_FLAGS) $(DEBUG_FLAGS) -x assembler-with-cpp -c "$<" -o "$@"

$(ARMOBJDIR)/channel.elf: $(ARM_OBJECTS) STM32H725xG_flash.ld makefile
	@printf 'Linking firmware\n'
	@$(CXX) $(LDFLAGS) $(ARM_LDFLAGS) $(ARM_OBJECTS) $(LDLIBS) -o "$@"
	@$(SIZE) "$@"

$(BINDIR)/channel.bin: $(ARMOBJDIR)/channel.elf
	@mkdir -p "$(@D)"
	@printf 'Creating %s\n' "$@"
	@$(OBJCOPY) -O binary "$<" "$@"

# ---- cleanup -----------------------------------------------------------------

clean:
	@rm -rf "$(OBJDIR)" "$(ARMOBJDIR)"
	@rm -f "$(BINDIR)/simulator" "$(BINDIR)/channel.bin"

# ---- dependencies ------------------------------------------------------------

-include $(OBJECTS:.o=.d) $(ARM_OBJECTS:.o=.d)
