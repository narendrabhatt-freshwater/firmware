#include "channel_led.h"
#include "main.h"

/* The RGB pins are plain GPIOs, so brightness uses 4-bit, 1 kHz software PWM.
 * That gives a 62.5 Hz frame while keeping ChannelConsole_Poll non-blocking. */

static volatile uint8_t s_level[3];
static uint8_t s_phase;
static volatile uint8_t s_controlled;
static uint32_t s_last_tick;

void ChannelLed_Init(void)
{
  s_level[0] = s_level[1] = s_level[2] = 0u;
  s_phase = 0u;
  s_controlled = 0u;
  s_last_tick = HAL_GetTick();
}

static uint8_t led_level(float color, float brightness)
{
  float scaled = color * brightness * 16.0f;
  return (uint8_t)(scaled + 0.5f);
}

int ChannelLed_Set(float red, float green, float blue, float brightness)
{
  s_level[0] = led_level(red, brightness);
  s_level[1] = led_level(green, brightness);
  s_level[2] = led_level(blue, brightness);
  s_controlled = 1u;
  return 0;
}

void ChannelLed_Task(void)
{
  static GPIO_TypeDef *const ports[3] = {RGB_R_GPIO_Port, RGB_G_GPIO_Port,
                                         RGB_B_GPIO_Port};
  static const uint16_t pins[3] = {RGB_R_Pin, RGB_G_Pin, RGB_B_Pin};
  uint32_t now;
  uint8_t i;

  if (!s_controlled) return;
  now = HAL_GetTick();
  if (now == s_last_tick) return;
  s_last_tick = now;
  s_phase = (uint8_t)((s_phase + 1u) & 0x0fu);
  for (i = 0u; i < 3u; ++i)
    HAL_GPIO_WritePin(ports[i], pins[i], s_phase < s_level[i] ? GPIO_PIN_SET
                                                              : GPIO_PIN_RESET);
}
