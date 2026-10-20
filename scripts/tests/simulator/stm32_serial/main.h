#pragma once
#include "../stm32/main.h"
#include <cstdlib>
using HAL_StatusTypeDef = int;
inline uint32_t now_ms;
inline uint32_t HAL_GetTick() { return now_ms; }
inline void HAL_NVIC_DisableIRQ(int) {}
enum { HAL_OK = 0, GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1, GPIO_PIN_12 = 4096,
       GPIO_MODE_INPUT, GPIO_MODE_AF_PP, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL,
       GPIO_PULLUP, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF8_UART5,
       UART_RXFIFO_THRESHOLD_1_8, UART5_IRQn, OTG_HS_IRQn };
struct GPIO_TypeDef { uint32_t BSRR; unsigned state; };
struct GPIO_InitTypeDef { unsigned Pin, Mode, Pull, Speed, Alternate; };
inline GPIO_TypeDef gpio, control_gpio;
#define GPIOC (&gpio)
#define RS485_CTL_GPIO_Port (&control_gpio)
#define RS485_CTL_Pin 1u
inline void HAL_GPIO_WritePin(GPIO_TypeDef *p, unsigned, unsigned value) { p->state = value; }
inline unsigned HAL_GPIO_ReadPin(GPIO_TypeDef *p, unsigned) { return p->state; }
inline void HAL_GPIO_Init(GPIO_TypeDef *, GPIO_InitTypeDef *) {}
struct TestUART { uint32_t ISR, RDR, ICR, CR1, RQR, TDR; };
inline TestUART uart;
#define UART5 (&uart)
enum { USART_ISR_RXNE_RXFNE=1, USART_ISR_TC=2, USART_ISR_TXE_TXFNF=4,
       USART_ISR_ORE=8, USART_ISR_FE=16, USART_ISR_NE=32,
       USART_ICR_ORECF=1, USART_ICR_FECF=2, USART_ICR_NECF=4,
       USART_ICR_PECF=8, USART_ICR_TCCF=16,
       USART_CR1_RXNEIE_RXFNEIE=1, USART_CR1_TCIE=2, USART_CR1_TXEIE_TXFNFIE=4,
       USART_RQR_RXFRQ=1 };
struct TestDebug { uint32_t DEMCR; };
struct TestDWT { uint32_t CYCCNT, CTRL; };
inline TestDebug test_debug;
inline TestDWT test_dwt;
#define CoreDebug (&test_debug)
#define DWT (&test_dwt)
#define CoreDebug_DEMCR_TRCENA_Msk 1u
#define DWT_CTRL_CYCCNTENA_Msk 1u
#define LED_R_GPIO_Port (&gpio)
#define LED_R_Pin 1u
#define LED_Y_GPIO_Port (&gpio)
#define LED_Y_Pin 1u
#define RGB_R_GPIO_Port (&gpio)
#define RGB_R_Pin 1u
#define RGB_G_GPIO_Port (&gpio)
#define RGB_G_Pin 1u
#define RGB_B_GPIO_Port (&gpio)
#define RGB_B_Pin 1u
#define HP_SW_GPIO_Port (&gpio)
#define HP_SW_Pin 1u
#define BP_SW_GPIO_Port (&gpio)
#define BP_SW_Pin 1u
#define LP_SW_GPIO_Port (&gpio)
#define LP_SW_Pin 1u
#define VCA_SW_GPIO_Port (&gpio)
#define VCA_SW_Pin 1u
#define BYPASS_SW_GPIO_Port (&gpio)
#define BYPASS_SW_Pin 1u
#define VCF_SW_GPIO_Port (&gpio)
#define VCF_SW_Pin 1u
#define SCF_SW_GPIO_Port (&gpio)
#define SCF_SW_Pin 1u
#define HP_CTL_GPIO_Port (&gpio)
#define HP_CTL_Pin 1u
