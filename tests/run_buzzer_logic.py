#!/usr/bin/env python3
"""Check the real buzzer driver with simulated timer/GPIO; no board required."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
stub = r"""
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef enum { HAL_OK, HAL_ERROR } HAL_StatusTypeDef;
typedef struct { uint32_t CFGR; } RCC_TypeDef;
typedef struct {
  uint32_t PSC, ARR, CCR3, CCMR2, CCER, CR1, EGR, SR, CNT, DIER;
} TIM_TypeDef;
typedef struct { uint32_t Pin, Mode, Pull, Speed, Alternate; } GPIO_InitTypeDef;
static RCC_TypeDef rcc;
static TIM_TypeDef timer;
static GPIO_InitTypeDef pin;
static uint32_t pclk, clocks, resets;
#define RCC (&rcc)
#define TIM2 (&timer)
#define GPIOA ((void *)1)
#define RCC_CFGR_PPRE1 (7U << 8)
#define TIM_CCMR2_OC3M_1 (1U << 5)
#define TIM_CCMR2_OC3M_2 (1U << 6)
#define TIM_CCMR2_OC3PE (1U << 3)
#define TIM_CCER_CC3E (1U << 8)
#define TIM_CR1_ARPE (1U << 7)
#define TIM_CR1_CEN 1U
#define TIM_EGR_UG 1U
#define GPIO_PIN_2 (1U << 2)
#define GPIO_MODE_AF_PP 2U
#define GPIO_PULLDOWN 2U
#define GPIO_SPEED_FREQ_LOW 0U
#define GPIO_AF1_TIM2 1U
#define __HAL_RCC_GPIOA_CLK_ENABLE() (clocks |= 1U)
#define __HAL_RCC_TIM2_CLK_ENABLE() (clocks |= 2U)
#define __HAL_RCC_TIM2_FORCE_RESET() (memset(&timer, 0, sizeof(timer)), ++resets)
#define __HAL_RCC_TIM2_RELEASE_RESET() ((void)0)
static uint32_t HAL_RCC_GetPCLK1Freq(void) { return pclk; }
static void HAL_GPIO_Init(void *port, GPIO_InitTypeDef *gpio) {
  assert(port == GPIOA && clocks == 3);
  /* PA2 must not switch to alternate function until PWM is running silently. */
  assert((timer.CR1 & TIM_CR1_CEN) && timer.CCR3 == 0);
  pin = *gpio;
}
"""
cases = r"""
int main(int argc, char **argv) {
  assert(argc == 2);
  unsigned scenario = (unsigned)atoi(argv[1]);
  pclk = scenario == 1 ? 85000000U : 170000000U;
  rcc.CFGR = scenario == 1 ? (4U << 8) : 0U;
  if (scenario == 2) pclk = 999999U;
  if (scenario == 3) pclk = 170000001U;
  Buzzer_Set(1);
  assert(clocks == 0 && timer.CCR3 == 0); /* Safe before initialization. */
  if (scenario >= 2) {
    assert(Buzzer_Init() == HAL_ERROR);
    Buzzer_Set(1);
    assert(clocks == 0 && timer.CCR3 == 0);
    return 0;
  }
  assert(Buzzer_Init() == HAL_OK);
  assert(pin.Pin == GPIO_PIN_2 && pin.Mode == GPIO_MODE_AF_PP);
  assert(pin.Alternate == GPIO_AF1_TIM2 && pin.Pull == GPIO_PULLDOWN);
  double hz = 170000000.0 / (timer.PSC + 1) / (timer.ARR + 1);
  assert(hz > 2390 && hz < 2410 && timer.CCR3 == 0);
  assert(timer.DIER == 0 && timer.CCER == TIM_CCER_CC3E);
  assert(timer.CCMR2 == ((6U << 4) | TIM_CCMR2_OC3PE));
  Buzzer_Set(1);
  double duty = (double)timer.CCR3 / (timer.ARR + 1);
  assert(duty > 0.49 && duty < 0.51);
  timer.CNT = 123;
  Buzzer_Set(1);
  assert(timer.CNT == 123 && resets == 1); /* Repeated gates preserve carrier. */
  Buzzer_Set(0);
  assert(timer.CCR3 == 0 && (timer.CR1 & TIM_CR1_CEN));
  assert(timer.CCER == TIM_CCER_CC3E); /* Silence actively drives PWM low. */
}
"""
with tempfile.TemporaryDirectory(prefix="yld-buzzer-tests-") as directory:
    path = pathlib.Path(directory)
    (path / "stm32g4xx_hal.h").write_text(stub)
    test_c = path / "buzzer_test.c"
    test_c.write_text((ROOT / "Core/Src/buzzer.c").read_text() + cases)
    executable = path / "buzzer_test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(path), "-I", str(ROOT / "Core/Inc"),
                    str(test_c), "-o", str(executable)], check=True)
    for scenario in range(4):
        subprocess.run([str(executable), str(scenario)], check=True)
print("PASS: PWM frequency/duty, silent startup, APB clock doubling, gates and invalid clocks")
