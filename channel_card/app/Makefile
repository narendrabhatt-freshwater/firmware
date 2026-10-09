################################################################################
# Freshwater Channel Card — standalone GNU Make build
################################################################################

# RS485, 8N1. Edit here, or override with: make BAUDRATE=3000000
BAUDRATE ?= 921600
CROSS_COMPILE ?= arm-none-eabi-
PYTHON ?= python3
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
.PHONY: all info clean FORCE

# ---- source lists (keep link order stable) -----------------------------------

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

APP_SOURCES := \
    app/cs4304.cpp \
    app/audio.cpp \
    app/channel.cpp \
    app/filter.cpp \
    app/oscillator.cpp \
    app/voice.cpp \
    app/samples.cpp \
    app/stream.cpp \
    app/usb.cpp

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
MCU_DEFINES := -DUSE_PWR_DIRECT_SMPS_SUPPLY -DUSE_HAL_DRIVER -DSTM32H725xx
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
	@mkdir -p "$(@D)"
	@$(PYTHON) -c 'import os, pathlib, re, subprocess, sys; baud = os.environ["BAUDRATE"]; valid = re.fullmatch(r"[1-9][0-9]*", baud) and int(baud) <= 4000000; sys.exit("BAUDRATE must be a positive integer no greater than 4000000") if not valid else None; p = pathlib.Path(sys.argv[1]); versions = [subprocess.check_output([c, "--version"]).decode() for c in sys.argv[2:]]; data = os.environ["BUILD_SETTINGS"] + "\n" + "".join(versions); p.write_text(data) if not p.exists() or p.read_text() != data else None' "$@" "$(CC)" "$(CXX)"

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

all: $(BINARY) $(OBJDIR)/$(NAME).hex
	@cat "$(INFO)"

# ---- firmware report (embedded to keep one build file) -----------------------

define REPORT_SCRIPT
import datetime
import hashlib
import pathlib
import re
import subprocess
import sys

elf, mapfile, binary, objdump, compiler, baud, output = sys.argv[1:]
regions = []
in_memory = False
for line in pathlib.Path(mapfile).read_text().splitlines():
    if line == "Memory Configuration":
        in_memory = True
    elif in_memory and line.startswith("*default*"):
        break
    elif in_memory:
        match = re.match(r"(\w+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)", line)
        if match:
            name, address, capacity = match.groups()
            origin = int(address, 16)
            regions.append([name, address, origin, int(capacity, 16), origin])
if not regions:
    raise SystemExit("No memory regions found in " + mapfile)
sections = []
section = None
for line in subprocess.check_output([objdump, "-h", elf], text=True).splitlines():
    match = re.match(r"\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)", line)
    if match:
        name, size, vma, lma = match.groups()
        section = (name, int(size, 16), int(vma, 16), int(lma, 16))
    elif section and "ALLOC" in line:
        name, size, vma, lma = section
        if size:
            sections.append((name, vma, size))
            addresses = [vma] + ([lma] if "LOAD" in line and lma != vma else [])
            for start in addresses:
                for region in regions:
                    if region[2] <= start < region[2] + region[3]:
                        region[4] = max(region[4], start + size)
                        break
        section = None
if not sections:
    raise SystemExit("No allocated ELF sections found in " + elf)
data = pathlib.Path(binary).read_bytes()
built = datetime.datetime.fromtimestamp(pathlib.Path(elf).stat().st_mtime, datetime.timezone.utc)
version = subprocess.check_output([compiler, "--version"], text=True).splitlines()[0]
lines = ["", "Channel Card firmware",
         f"  Binary:      bin/channel_MCU.bin ({len(data)} bytes)",
         "  Target:      STM32H725, Cortex-M7, Release",
         f"  RS485:       {baud} baud, 8N1",
         f"  Toolchain:   {version}",
         f"  Built:       {built:%Y-%m-%d %H:%M:%S UTC}",
         f"  SHA256:      {hashlib.sha256(data).hexdigest()}", "",
         "Memory region  Origin      Used (B)    Capacity (B)  Free (B)    Used"]
for name, address, origin, capacity, high in regions:
    used = high - origin
    percent = (10000 * used + capacity // 2) // capacity
    lines.append(f"{name:<15}{address:<12}{used:<12}{capacity:<14}{capacity-used:<12}{percent//100}.{percent%100:02d}%")
lines += ["", "Allocated sections       Address     Bytes"]
lines += [f"{name:<25}0x{address:08x}  {size}" for name, address, size in sections]
lines += ["", "RAM figures include linker reservations and alignment, not runtime peaks.",
          "Initialized RAM also occupies flash; heap/stack reserve is shown above."]
pathlib.Path(output).write_text("\n".join(lines) + "\n")
endef
export REPORT_SCRIPT

$(INFO): $(BUILD_BINARY) $(MAKEFILE)
	@$(PYTHON) -c "$$REPORT_SCRIPT" "$(ELF)" "$(MAP)" "$(BUILD_BINARY)" "$(OBJDUMP)" "$(CXX)" "$(BAUDRATE)" "$@.tmp"
	@mv "$@.tmp" "$@"

# Report the last successful build without compiling or changing its baud rate.
info:
	@test -f "$(INFO)" && test -f "$(BINARY)" || \
	    { echo "No firmware report available. Run make first."; exit 1; }
	@cmp -s "$(BINARY)" "$(BUILD_BINARY)" || \
	    { echo "Output binary differs from the last build. Run make first."; exit 1; }
	@cat "$(INFO)"

clean:
	@rm -rf "$(OBJDIR)"
	@rm -f "$(BINARY)" "$(BINARY).tmp"

-include $(OBJECTS:.o=.d)
