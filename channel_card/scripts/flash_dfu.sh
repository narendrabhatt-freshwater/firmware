#!/bin/bash
#
# flash_dfu.sh — Flash STM32H725 via DFU mode from RK3568
#
# This script controls the STM32H725 BOOT0 and NRST pins via RK3568 GPIO
# to enter DFU (Device Firmware Upgrade) mode, then flashes the firmware
# using dfu-util.
#
# Hardware connections:
#   RK3568 GPIO4_D2 (gpio154) --> STM32H725 BOOT0 (active high for DFU)
#   RK3568 GPIO0_C7 (gpio23)  --> STM32H725 NRST  (active low reset)
#
# GPIO number calculation:
#   GPIO4_D2 = 4*32 + 3*8 + 2 = 154
#   GPIO0_C7 = 0*32 + 2*8 + 7 = 23
#
# DFU entry sequence:
#   1. Assert BOOT0 HIGH (enter bootloader on next reset)
#   2. Assert NRST LOW (hold in reset)
#   3. Release NRST HIGH (STM32 boots into DFU bootloader)
#   4. Wait for USB DFU device to enumerate
#   5. Flash firmware with dfu-util
#   6. Release BOOT0 LOW (normal boot on next reset)
#   7. Reset STM32 to run new firmware
#
# Usage:
#   ./flash_dfu.sh <firmware.bin>
#   ./flash_dfu.sh /tmp/channel_MCU.bin
#

set -e

# ─── Configuration ───────────────────────────────────────────────────────────

# GPIO pin numbers (sysfs numbering)
GPIO_BOOT0=154    # GPIO4_D2 → STM32 BOOT0
GPIO_NRST=23      # GPIO0_C7 → STM32 NRST

# STM32H725 DFU USB IDs (ST built-in bootloader)
DFU_VID="0483"
DFU_PID="df11"

# DFU-util alt setting for internal flash
# STM32H725: Internal Flash starts at 0x08000000
DFU_ALT=0
DFU_ADDR="0x08000000"

# Timing (seconds)
RESET_PULSE=0.1       # Reset pulse width
DFU_SETTLE=0.5        # Time after reset release for bootloader to start
DFU_ENUM_TIMEOUT=10   # Max seconds to wait for USB DFU device

# ─── Functions ───────────────────────────────────────────────────────────────

log() {
    echo "[$(date '+%H:%M:%S')] $*"
}

error() {
    echo "[$(date '+%H:%M:%S')] ERROR: $*" >&2
    cleanup_gpio
    exit 1
}

# Export and configure a GPIO pin as output
setup_gpio() {
    local gpio=$1
    local direction=$2
    local value=$3

    if [ ! -d "/sys/class/gpio/gpio${gpio}" ]; then
        echo "$gpio" > /sys/class/gpio/export 2>/dev/null || true
        sleep 0.1
    fi

    echo "$direction" > "/sys/class/gpio/gpio${gpio}/direction"

    if [ -n "$value" ]; then
        echo "$value" > "/sys/class/gpio/gpio${gpio}/value"
    fi
}

# Set GPIO value
gpio_set() {
    local gpio=$1
    local value=$2
    echo "$value" > "/sys/class/gpio/gpio${gpio}/value"
}

# Read GPIO value
gpio_get() {
    local gpio=$1
    cat "/sys/class/gpio/gpio${gpio}/value"
}

# Unexport GPIOs on exit
cleanup_gpio() {
    log "Keeping GPIOs driven (BOOT0=LOW, NRST=HIGH)..."
    # Ensure BOOT0 is LOW (normal boot)
    gpio_set $GPIO_BOOT0 0 2>/dev/null || true
    # Ensure NRST is HIGH (not in reset)
    gpio_set $GPIO_NRST 1 2>/dev/null || true
}

# Wait for DFU device to appear on USB
wait_for_dfu() {
    local timeout=$1
    local elapsed=0

    log "Waiting for DFU device (${DFU_VID}:${DFU_PID})..."

    while [ "$elapsed" -lt "$timeout" ]; do
        if dfu-util -l 2>/dev/null | grep -q "${DFU_VID}:${DFU_PID}"; then
            log "DFU device found!"
            return 0
        fi
        sleep 0.5
        elapsed=$((elapsed + 1))
    done

    return 1
}

# Enter DFU mode
enter_dfu_mode() {
    log "Entering DFU mode..."

    # Step 1: Assert BOOT0 HIGH (STM32 will boot into bootloader)
    log "  BOOT0 → HIGH"
    gpio_set $GPIO_BOOT0 1

    # Step 2: Assert NRST LOW (hold in reset)
    log "  NRST  → LOW (reset)"
    gpio_set $GPIO_NRST 0
    sleep $RESET_PULSE

    # Step 3: Release NRST (STM32 exits reset → enters DFU bootloader)
    log "  NRST  → HIGH (release)"
    gpio_set $GPIO_NRST 1
    sleep $DFU_SETTLE

    log "DFU mode entry sequence complete."
}

# Reset STM32 for normal boot
reset_normal() {
    log "Resetting for normal boot..."

    # Release BOOT0 (LOW = boot from flash)
    gpio_set $GPIO_BOOT0 0
    sleep 0.1

    # Pulse NRST
    gpio_set $GPIO_NRST 0
    sleep $RESET_PULSE
    gpio_set $GPIO_NRST 1

    log "STM32 reset — running new firmware."
}

# ─── Main ────────────────────────────────────────────────────────────────────

# Check arguments
if [ $# -lt 1 ]; then
    echo "Usage: $0 <firmware.bin>"
    echo ""
    echo "Examples:"
    echo "  $0 /tmp/channel_MCU.bin"
    echo "  $0 channel_MCU.bin"
    echo ""
    echo "Options:"
    echo "  --dfu-only    Enter DFU mode without flashing"
    echo "  --reset-only  Reset STM32 for normal boot"
    exit 1
fi

# Handle special options
case "$1" in
    --dfu-only)
        log "=== DFU Mode Entry Only ==="
        setup_gpio $GPIO_BOOT0 "out" 0
        setup_gpio $GPIO_NRST  "out" 1
        enter_dfu_mode
        log "STM32 is now in DFU mode. Run dfu-util manually."
        log "To reset: $0 --reset-only"
        exit 0
        ;;
    --reset-only)
        log "=== Reset Only ==="
        setup_gpio $GPIO_BOOT0 "out" 0
        setup_gpio $GPIO_NRST  "out" 1
        reset_normal
        cleanup_gpio
        exit 0
        ;;
esac

FIRMWARE="$1"

# Validate firmware file
if [ ! -f "$FIRMWARE" ]; then
    echo "Error: firmware file not found: $FIRMWARE"
    exit 1
fi

FIRMWARE_SIZE=$(stat -c %s "$FIRMWARE" 2>/dev/null || stat -f %z "$FIRMWARE" 2>/dev/null)
log "Firmware: $FIRMWARE ($FIRMWARE_SIZE bytes)"

# Check dfu-util is installed
if ! command -v dfu-util &>/dev/null; then
    error "dfu-util not found. Install with: apt-get install dfu-util"
fi

# Set up trap for clean exit
trap cleanup_gpio EXIT INT TERM

log "=== STM32H725 DFU Flash ==="

# Step 1: Setup GPIOs
log "Setting up GPIOs..."
setup_gpio $GPIO_BOOT0 "out" 0   # BOOT0 = LOW (default: normal boot)
setup_gpio $GPIO_NRST  "out" 1   # NRST  = HIGH (default: not in reset)

# Step 2: Enter DFU mode
enter_dfu_mode

# Step 3: Wait for DFU device
if ! wait_for_dfu $DFU_ENUM_TIMEOUT; then
    error "DFU device not found after ${DFU_ENUM_TIMEOUT}s. Check USB connection."
fi

# Step 4: Flash firmware
log "Flashing firmware..."
set +e
dfu-util -a $DFU_ALT -d "${DFU_VID}:${DFU_PID}" -s "${DFU_ADDR}:leave" -D "$FIRMWARE"
DFU_EXIT_CODE=$?
set -e

if [ $DFU_EXIT_CODE -eq 0 ]; then
    log "Flash complete!"
else
    log "dfu-util completed with code $DFU_EXIT_CODE (often safe to ignore)."
fi

# Step 5: Reset for normal boot
sleep 0.5
reset_normal

log "=== Done ==="
