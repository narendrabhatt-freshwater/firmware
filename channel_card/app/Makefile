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

################################################################################
# Freshwater Channel Card — standalone GNU Make build
################################################################################

# RS485, 8N1. Edit here, or override with: make board BAUDRATE=3000000
BAUDRATE ?= 921600
CROSS_COMPILE ?= arm-none-eabi-
SHA256 ?= shasum -a 256
CC := $(CROSS_COMPILE)gcc
CXX := $(CROSS_COMPILE)g++
AR := $(CROSS_COMPILE)ar
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump

MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
ROOT := $(abspath $(dir $(MAKEFILE))/..)
OBJDIR := $(ROOT)/build/release
BINDIR := $(ROOT)/bin
NAME := channel_MCU
ELF := $(OBJDIR)/$(NAME).elf
MAP := $(OBJDIR)/$(NAME).map
BUILD_BINARY := $(OBJDIR)/$(NAME).bin
BINARY := $(BINDIR)/$(NAME).bin
INFO := $(OBJDIR)/$(NAME).info
CONFIG := $(OBJDIR)/build.config
LINKER_SCRIPT := $(ROOT)/STM32H725xG_flash.ld
BERRY := berry_runtime/third_party/berry

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
.PHONY: all sim board clean FORCE

# ---- source lists (keep link order stable) ----------------------------------

CORE_SOURCES := \
    core/src/main.c \
    core/src/gpio.c \
    core/src/dma.c \
    core/src/i2c.c \
    core/src/i2s.c \
    core/src/sai.c \
    core/src/tim.c \
    core/src/usart.c \
    core/src/usb_otg.c \
    core/src/stm32h7xx_it.c \
    core/src/stm32h7xx_hal_msp.c \
    core/src/sysmem.c \
    core/src/syscalls.c \
    startup_stm32h725xx.s

APP_SOURCES := app/audio.cpp app/channel.cpp app/filter.cpp \
               app/oscillator.cpp app/voice.cpp app/samples.cpp \
               app/stream.cpp app/usb.cpp app/transport.cpp app/platform.cpp \
               app/cs4304.cpp app/audio_dac.cpp \
               app/audio_i2s.cpp app/board_stm32.cpp app/usb_device.cpp

DRIVER_SOURCES := \
    core/src/system_stm32h7xx.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_cortex.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_i2c.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_i2c_ex.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_rcc.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_rcc_ex.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_flash.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_flash_ex.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_gpio.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_hsem.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_dma.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_dma_ex.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_mdma.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_pwr.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_pwr_ex.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_exti.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_i2s.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_i2s_ex.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_sai.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_sai_ex.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_tim.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_tim_ex.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_uart.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_uart_ex.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_pcd.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_hal_pcd_ex.c \
    drivers/stm32h7xx_hal_driver/src/stm32h7xx_ll_usb.c

VM_SOURCES := berry_runtime/vm.c berry_runtime/vm_source.c
BACKEND_SOURCES := berry_runtime/berry_backend.c
BERRY_SOURCES := $(patsubst $(ROOT)/%,%,$(sort $(wildcard $(ROOT)/$(BERRY)/src/*.c))) \
                 berry_runtime/berry_runtime_modtab.c

# Include the extension in object names so C, C++ and assembly cannot collide.
CORE_OBJECTS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(CORE_SOURCES)))
APP_OBJECTS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(APP_SOURCES)))
DRIVER_OBJECTS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(DRIVER_SOURCES)))
VM_OBJECTS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(VM_SOURCES)))
BACKEND_OBJECTS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(BACKEND_SOURCES)))
BERRY_OBJECTS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(BERRY_SOURCES)))
OBJECTS := $(CORE_OBJECTS) $(APP_OBJECTS) $(DRIVER_OBJECTS) \
           $(VM_OBJECTS) $(BACKEND_OBJECTS) $(BERRY_OBJECTS)
VM_LIB := $(OBJDIR)/libberry_vm.a
BACKEND_LIB := $(OBJDIR)/libscript_runtime_berry_backend.a
BERRY_LIB := $(OBJDIR)/libberry_runtime.a

# ---- compiler and linker settings -------------------------------------------

CPU_FLAGS := -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard
COMMON_FLAGS := $(CPU_FLAGS) -Wall -fdata-sections -ffunction-sections \
                -fstack-usage -O3 -g0
CFLAGS := $(COMMON_FLAGS)
CXXFLAGS := $(COMMON_FLAGS) -std=c++17 -fno-rtti -fno-exceptions \
            -fno-threadsafe-statics
ASFLAGS := $(CPU_FLAGS) -x assembler-with-cpp -O3 -DNDEBUG
MCU_DEFINES := -DCHANNEL_ENABLE_CYCLE_PROFILING \
               -DUSE_PWR_DIRECT_SMPS_SUPPLY -DUSE_HAL_DRIVER -DSTM32H725xx
MCU_INCLUDES := -I$(ROOT)/core/inc \
                -I$(ROOT)/drivers/stm32h7xx_hal_driver/inc \
                -I$(ROOT)/drivers/stm32h7xx_hal_driver/inc/legacy \
                -I$(ROOT)/drivers/cmsis/device/st/stm32h7xx/include \
                -I$(ROOT)/drivers/cmsis/include
APP_INCLUDES := -I$(ROOT)/app -I$(ROOT)/berry_runtime $(MCU_INCLUDES) \
                -I$(ROOT)/$(BERRY)/src
LDFLAGS := $(CPU_FLAGS) -T$(LINKER_SCRIPT) --specs=nano.specs \
           -Wl,-Map=$(MAP) -Wl,--gc-sections -Wl,--print-memory-usage \
           -Wl,-u,_scanf_float -Wl,-u,_printf_float

$(CORE_OBJECTS) $(APP_OBJECTS): PRIVATE_CPPFLAGS := $(MCU_DEFINES) $(APP_INCLUDES) -DFW_RS485_BAUD=$(BAUDRATE)u
$(CORE_OBJECTS): PRIVATE_CFLAGS := -std=gnu11
$(DRIVER_OBJECTS): PRIVATE_CPPFLAGS := $(MCU_DEFINES) $(MCU_INCLUDES)
$(DRIVER_OBJECTS): PRIVATE_CFLAGS := -std=gnu11
$(VM_OBJECTS): PRIVATE_CPPFLAGS := -I$(ROOT)/berry_runtime
$(VM_OBJECTS): PRIVATE_CFLAGS := -std=c11 -Wextra -Werror
$(BACKEND_OBJECTS): PRIVATE_CPPFLAGS := -I$(ROOT)/berry_runtime -I$(ROOT)/$(BERRY)/src
$(BACKEND_OBJECTS): PRIVATE_CFLAGS := -std=c11 -Wextra -Werror -pedantic
$(BERRY_OBJECTS): PRIVATE_CPPFLAGS := -I$(ROOT)/$(BERRY)/src -I$(ROOT)/berry_runtime
$(BERRY_OBJECTS): PRIVATE_CFLAGS := -std=c11 -w

# Check settings on every build, but only change the stamp when settings change.
# This also detects a compiler upgrade at the same executable path.
export BAUDRATE
export BUILD_SETTINGS = $(CC)|$(CXX)|$(AR)|$(OBJCOPY)|$(OBJDUMP)|$(CPPFLAGS)|$(CFLAGS)|$(CXXFLAGS)|$(ASFLAGS)|$(LDFLAGS)|$(BAUDRATE)|$(OBJECTS)
$(CONFIG): FORCE
	@set -eu; \
	awk 'BEGIN { baud = ENVIRON["BAUDRATE"]; \
	    if (baud !~ /^[1-9][0-9]*$$/ || baud + 0 > 4000000) { \
	        print "BAUDRATE must be a positive integer no greater than 4000000"; exit 1; \
	    } }'; \
	mkdir -p "$(@D)"; \
	trap 'rm -f "$@.tmp"' EXIT HUP INT TERM; \
	{ printf '%s\n' "$$BUILD_SETTINGS"; \
	  "$(CC)" --version; printf '\n'; "$(CXX)" --version; printf '\n'; \
	} > "$@.tmp"; \
	if ! cmp -s "$@.tmp" "$@"; then mv "$@.tmp" "$@"; fi

FORCE:

# ---- compile and link -------------------------------------------------------

$(OBJECTS): $(CONFIG) $(MAKEFILE)

$(OBJDIR)/%.c.o: $(ROOT)/%.c
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(PRIVATE_CPPFLAGS) $(CFLAGS) $(PRIVATE_CFLAGS) -MMD -MP -MF "$(@:.o=.d)" -c "$<" -o "$@"

$(OBJDIR)/%.cpp.o: $(ROOT)/%.cpp
	@mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(PRIVATE_CPPFLAGS) $(CXXFLAGS) -MMD -MP -MF "$(@:.o=.d)" -c "$<" -o "$@"

$(OBJDIR)/%.s.o: $(ROOT)/%.s
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(PRIVATE_CPPFLAGS) $(ASFLAGS) -MMD -MP -MF "$(@:.o=.d)" -c "$<" -o "$@"

$(VM_LIB): $(VM_OBJECTS) $(MAKEFILE)
	@rm -f "$@"
	$(AR) qc "$@" $(VM_OBJECTS)
	$(AR) s "$@"

$(BACKEND_LIB): $(BACKEND_OBJECTS) $(MAKEFILE)
	@rm -f "$@"
	$(AR) qc "$@" $(BACKEND_OBJECTS)
	$(AR) s "$@"

$(BERRY_LIB): $(BERRY_OBJECTS) $(MAKEFILE)
	@rm -f "$@"
	$(AR) qc "$@" $(BERRY_OBJECTS)
	$(AR) s "$@"

$(ELF): $(CORE_OBJECTS) $(APP_OBJECTS) $(DRIVER_OBJECTS) $(BACKEND_LIB) $(BERRY_LIB) $(VM_LIB) $(LINKER_SCRIPT) $(MAKEFILE)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(CORE_OBJECTS) $(APP_OBJECTS) $(DRIVER_OBJECTS) -o "$@" -lm $(BACKEND_LIB) $(BERRY_LIB) -lm $(VM_LIB) -lm

$(BUILD_BINARY): $(ELF)
	$(OBJCOPY) -O binary "$<" "$@"

# Optional compatibility output for scripts/fw's CubeProgrammer path.
$(OBJDIR)/$(NAME).hex: $(ELF)
	$(OBJCOPY) -O ihex "$<" "$@"

# Publish only after binary conversion and report generation succeed.
$(BINARY): $(BUILD_BINARY) $(INFO)
	@mkdir -p "$(@D)"
	@cp "$(BUILD_BINARY)" "$@.tmp"
	@mv "$@.tmp" "$@"

all: sim

board: $(BINARY) $(OBJDIR)/$(NAME).hex
	@cat "$(INFO)"

# ---- firmware report --------------------------------------------------------

# Keep reporting self-contained so an SVN checkout needs no Python helpers.
# POSIX awk handles the map and ELF section listing on macOS and Linux.
$(INFO): $(BUILD_BINARY) $(MAKEFILE)
	@set -eu; \
	trap 'rm -f "$@.tmp" "$@.sections" "$@.version" "$@.hash"' EXIT HUP INT TERM; \
	"$(OBJDUMP)" -h "$(ELF)" > "$@.sections"; \
	"$(CXX)" --version > "$@.version"; \
	$(SHA256) "$(BUILD_BINARY)" > "$@.hash"; \
	{ printf '\nChannel Card firmware\n'; \
	  printf '  Binary:      bin/$(NAME).bin (%s bytes)\n' "$$(wc -c < "$(BUILD_BINARY)" | tr -d ' ')"; \
	  printf '  Target:      STM32H725, Cortex-M7, Release\n'; \
	  printf '  RS485:       %s baud, 8N1\n' "$$BAUDRATE"; \
	  printf '  Toolchain:   %s\n' "$$(head -n 1 "$@.version")"; \
	  printf '  Reported:    %s\n' "$$(date -u '+%Y-%m-%d %H:%M:%S UTC')"; \
	  printf '  SHA256:      %s\n\n' "$$(awk '{print $$1}' "$@.hash")"; \
	  awk 'function hex(s, n,i,d) { \
	      sub(/^0[xX]/,"",s); s=tolower(s); n=0; \
	      for(i=1;i<=length(s);i++) { d=index("0123456789abcdef",substr(s,i,1))-1; n=n*16+d; } \
	      return n; \
	  } \
	  function allocate(address,size, i) { \
	      for(i=1;i<=regions;i++) \
	          if(address>=origin[i] && address<origin[i]+capacity[i]) { \
	              if(address+size>high[i]) high[i]=address+size; break; \
	          } \
	  } \
	  FNR==NR { \
	      if($$0=="Memory Configuration") memory=1; \
	      else if(memory && $$1=="*default*") memory=0; \
	      else if(memory && $$2 ~ /^0x/ && $$3 ~ /^0x/) { \
	          ++regions; name[regions]=$$1; address[regions]=$$2; \
	          origin[regions]=hex($$2); capacity[regions]=hex($$3); high[regions]=origin[regions]; \
	      } \
	      next; \
	  } \
	  $$1 ~ /^[0-9]+$$/ && NF>=5 { section=$$2; size=hex($$3); vma=hex($$4); lma=hex($$5); pending=1; next; } \
	  pending && /ALLOC/ { \
	      if(size) { \
	          ++sections; section_name[sections]=section; section_vma[sections]=vma; section_size[sections]=size; \
	          allocate(vma,size); if(/LOAD/ && lma!=vma) allocate(lma,size); \
	      } \
	      pending=0; \
	  } \
	  END { \
	      if(!regions || !sections) { print "No memory regions or allocated ELF sections found"; exit 1; } \
	      print "Memory region  Origin      Used (B)    Capacity (B)  Free (B)    Used"; \
	      for(i=1;i<=regions;i++) { \
	          used=high[i]-origin[i]; percent=int((10000*used+capacity[i]/2)/capacity[i]); \
	          printf "%-15s%-12s%-12d%-14d%-12d%d.%02d%%\n",name[i],address[i],used,capacity[i],capacity[i]-used,int(percent/100),percent%100; \
	      } \
	      print "\nAllocated sections       Address     Bytes"; \
	      for(i=1;i<=sections;i++) printf "%-25s0x%08x  %d\n",section_name[i],section_vma[i],section_size[i]; \
	      print "\nRAM figures include linker reservations and alignment, not runtime peaks."; \
	      print "Initialized RAM also occupies flash; heap/stack reserve is shown above."; \
	  }' "$(MAP)" "$@.sections"; \
	} > "$@.tmp"; \
	mv "$@.tmp" "$@"

# Host simulator: the same application sources behind virtual ports. Built
# with the host compiler by sim/Makefile; it does not use the Arm toolchain.
sim:
	@test -f "$(ROOT)/sim/Makefile" || \
	    { echo "The simulator sources (sim/) are not part of this package."; exit 1; }
	@$(MAKE) -C "$(ROOT)/sim"

clean:
	@rm -rf "$(OBJDIR)"
	@rm -f "$(BINARY)" "$(BINARY).tmp"

-include $(OBJECTS:.o=.d)
