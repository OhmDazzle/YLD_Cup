#include "buzzer.h"

#define BUZZER_COUNTER_HZ 1000000U
#define BUZZER_TONE_HZ       2400U
#define BUZZER_PERIOD_TICKS \
  ((BUZZER_COUNTER_HZ + BUZZER_TONE_HZ / 2U) / BUZZER_TONE_HZ)

static uint8_t ready;

HAL_StatusTypeDef Buzzer_Init(void)
{
  GPIO_InitTypeDef gpio = {0};
  uint32_t timer_hz = HAL_RCC_GetPCLK1Freq();

  /* APB1 分频不为 1 时，定时器时钟为 PCLK1 的两倍。 */
  if ((RCC->CFGR & RCC_CFGR_PPRE1) != 0U) timer_hz *= 2U;
  if (timer_hz < BUZZER_COUNTER_HZ ||
      timer_hz % BUZZER_COUNTER_HZ != 0U ||
      timer_hz / BUZZER_COUNTER_HZ > 65536U) return HAL_ERROR;

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_TIM2_CLK_ENABLE();
  __HAL_RCC_TIM2_FORCE_RESET();
  __HAL_RCC_TIM2_RELEASE_RESET();
  TIM2->PSC = timer_hz / BUZZER_COUNTER_HZ - 1U;
  TIM2->ARR = BUZZER_PERIOD_TICKS - 1U;
  TIM2->CCR3 = 0U;
  /* CH3 PWM mode 1、高电平有效、比较值/周期预装载；不启用中断或 DMA。 */
  TIM2->CCMR2 = TIM_CCMR2_OC3M_1 | TIM_CCMR2_OC3M_2 | TIM_CCMR2_OC3PE;
  TIM2->CCER = TIM_CCER_CC3E;
  TIM2->CR1 = TIM_CR1_ARPE;
  TIM2->EGR = TIM_EGR_UG;
  TIM2->SR = 0U;
  /* 先以 0% 占空比启动，再交出 PA2 控制权，避免上电误鸣。 */
  TIM2->CR1 |= TIM_CR1_CEN;

  gpio.Pin = GPIO_PIN_2;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_PULLDOWN;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  gpio.Alternate = GPIO_AF1_TIM2;
  HAL_GPIO_Init(GPIOA, &gpio);
  ready = 1U;
  return HAL_OK;
}

void Buzzer_Set(uint8_t on)
{
  if (ready == 0U) return;
  /* 定时器持续运行；CCR=0 确保 PA2 输出低电平，Q2 关断。
   * 比较值预装载在下个周期生效（最长约 417us），重复设置不重启载波。 */
  TIM2->CCR3 = on ? BUZZER_PERIOD_TICKS / 2U : 0U;
}
