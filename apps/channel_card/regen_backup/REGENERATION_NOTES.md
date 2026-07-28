# CubeMX Regeneration Checklist

Before regenerating from channel_MCU.ioc, know what survives and what doesn't.

## SAFE (in USER CODE sections or .ioc — survives regeneration)

- main.c: CS4304 init, MasterMask, tone enable, I2S2 boot self-test (USER CODE 2)
- usbd_audio_if.c / .h: ALL custom audio code — buffers, tone generator,
  FIFO pump, I2S2_Start, Audio_FixI2S2, callbacks (USER CODE sections)
- stm32h7xx_it.c: SPI2_IRQHandler + TIM7_IRQHandler (moved into USER CODE 1)
- i2s.c: SPI2 NVIC enable (USER CODE SPI2_MspInit 1)
- Pin speeds: PA8 (MCLK, VERY_HIGH) and PA4/PA5/PA7/PC1 (I2S, HIGH) — now
  set in the .ioc, will regenerate correctly
- DMA: SPI2_TX on DMA1_Stream1 (.ioc matches code)
- cs4304.c / cs4304.h: not CubeMX-owned, untouched by regeneration

## AT RISK — RESTORE FROM THIS FOLDER AFTER REGENERATING

1. **Middlewares/.../AUDIO/Src/usbd_audio.c** — heavily customized:
   mono/32-bit/96 kHz descriptors, volume range −50..0 dB, volume GET/SET
   handlers, DataOut packet handling (buffer-overflow fix: always receive
   at buffer[0]). CubeMX copies a stock file over this. Restore usbd_audio.c
   from regen_backup/ after every regeneration.
2. **Middlewares/.../AUDIO/Inc/usbd_audio.h** — vol_cur/min/max/res handle
   fields, packet-size macros for mono 32-bit. Restore from backup.
3. **USB_DEVICE/Target/usbd_conf.h** — USBD_AUDIO_FREQ 96000U. CubeMX may
   reset it to 48000 (or wherever the MX USB settings point). Verify/restore.
4. **STM32H725XG_FLASH.ld** — the `.dma_buffer (NOLOAD)` section in RAM_D1
   (AXI SRAM) that holds the I2S DMA buffers (DMA1 cannot reach DTCM).
   If CubeMX rewrites the linker script, the build breaks. Restore from backup.

## Quick restore after regen

cp regen_backup/usbd_audio.c Middlewares/ST/STM32_USB_Device_Library/Class/AUDIO/Src/
cp regen_backup/usbd_audio.h Middlewares/ST/STM32_USB_Device_Library/Class/AUDIO/Inc/
cp regen_backup/usbd_conf.h  USB_DEVICE/Target/
cp regen_backup/STM32H725XG_FLASH.ld .

Then rebuild and diff-check that nothing else changed unexpectedly.
Keep this folder updated: re-copy the four files here whenever they are
modified intentionally.
