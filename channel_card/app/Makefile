################################################################################
#                         Freshwater Channel Card/Voice Board                  #
################################################################################

# ---- build settings ---------------------------------------------------------

# RS485, 8N1. Edit here, or override with: make BAUDRATE=3000000
BAUDRATE                ?= 921600
CMAKE                   ?= cmake

ROOT                    := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
OBJDIR                  := $(ROOT)/build/release
BINDIR                  := $(ROOT)/bin
NAME                    := channel_MCU
BINARY                  := $(BINDIR)/$(NAME).bin
INFO                    := $(OBJDIR)/$(NAME).info

.DEFAULT_GOAL           := all
.PHONY: all info clean

# ---- primary build target ---------------------------------------------------

all:
	@$(CMAKE) -S "$(ROOT)" -B "$(OBJDIR)" -G "Unix Makefiles" \
	    -DCMAKE_TOOLCHAIN_FILE="$(ROOT)/cmake/gcc-arm-none-eabi.cmake" \
	    -DCMAKE_BUILD_TYPE=Release -DFW_RS485_BAUD="$(BAUDRATE)"
	@$(MAKE) -C "$(OBJDIR)" --no-print-directory
	@$(CMAKE) -E make_directory "$(BINDIR)"
	@$(CMAKE) -E copy_if_different "$(OBJDIR)/$(NAME).bin" "$(BINARY)"
	@$(CMAKE) -E cat "$(INFO)"

# ---- firmware information ---------------------------------------------------

# Report the last successful build; do not reconfigure or change its baud rate.
info:
	@test -f "$(INFO)" && test -f "$(BINARY)" || \
	    { echo "No firmware report available. Run make first."; exit 1; }
	@cmp -s "$(BINARY)" "$(OBJDIR)/$(NAME).bin" || \
	    { echo "Output binary differs from the last build. Run make first."; exit 1; }
	@$(CMAKE) -E cat "$(INFO)"

# ---- cleanup ----------------------------------------------------------------

clean:
	@$(CMAKE) -E rm -rf "$(OBJDIR)"
	@$(CMAKE) -E rm -f "$(BINARY)"
