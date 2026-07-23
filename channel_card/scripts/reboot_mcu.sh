#!/bin/bash
# reboot_mcu.sh — Reset STM32H725 and run normal firmware from RK3568
#
# Hardware connections:
#   RK3568 GPIO4_D2 (gpio154) --> STM32 BOOT0 (active HIGH)
#   RK3568 GPIO0_C7 (gpio23)  --> STM32 NRST  (active LOW)

GPIO_BOOT0=154
GPIO_NRST=23

setup_gpio() {
    local gpio=$1
    if [ ! -d "/sys/class/gpio/gpio${gpio}" ]; then
        echo "$gpio" > /sys/class/gpio/export 2>/dev/null || true
        sleep 0.1
    fi
    echo "out" > "/sys/class/gpio/gpio${gpio}/direction"
}

echo "Setting up GPIOs..."
setup_gpio $GPIO_BOOT0
setup_gpio $GPIO_NRST

echo "Resetting MCU to normal boot..."
# Set BOOT0 Low (normal flash boot)
echo "0" > "/sys/class/gpio/gpio${GPIO_BOOT0}/value"
# Pulse Reset (LOW then HIGH)
echo "0" > "/sys/class/gpio/gpio${GPIO_NRST}/value"
sleep 0.1
echo "1" > "/sys/class/gpio/gpio${GPIO_NRST}/value"

# Clean up GPIO exports
echo "$GPIO_BOOT0" > /sys/class/gpio/unexport 2>/dev/null || true
echo "$GPIO_NRST" > /sys/class/gpio/unexport 2>/dev/null || true

echo "MCU rebooted successfully."
