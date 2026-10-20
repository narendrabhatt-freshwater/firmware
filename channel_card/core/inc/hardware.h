#ifndef CHANNEL_HARDWARE_H
#define CHANNEL_HARDWARE_H

// The Makefile overrides the UART rate with BAUDRATE.
#ifndef FW_RS485_BAUD
#define FW_RS485_BAUD 921600u
#endif

#define BUF_COUNT 2
#define AUDIO_BUFFER_FRAMES 48u
#define AUDIO_BUFFER_CHANNELS 2u

// STM32 storage sections are defined in STM32H725xG_flash.ld.
#define CHANNEL_ATTACK_STORAGE __attribute__((used, section(".attack_bank"), aligned(4)))
#define CHANNEL_VM_STORAGE __attribute__((aligned(8), section(".vm_arena")))
#define CHANNEL_AUDIO_STORAGE __attribute__((aligned(4), section(".dma_buffer")))

#endif
