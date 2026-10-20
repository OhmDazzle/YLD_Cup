#!/usr/bin/env python3
"""Host checks of the real shared test functions; no STM32 hardware required."""
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
source = (ROOT / "TEST_MODULES/test_board.c").read_text()
header = (ROOT / "TEST_MODULES/test_board.h").read_text().replace(
    '#include "stm32g4xx_hal.h"', '#include <stdint.h>'
)


def function(name):
    """Extract an entire function body, rather than copying its implementation."""
    match = re.search(r"^(?:uint16_t|uint8_t|void|bool) " + name + r"\([^;]+?\)\n\{", source, re.M)
    if not match:
        raise RuntimeError(f"Cannot find {name}")
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


stub = r"""
#include <assert.h>
#include <math.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
typedef struct { unsigned pins, out; } GPIO_TypeDef;
typedef struct { unsigned channel; } ADC_HandleTypeDef;
typedef struct { int unused; } DAC_HandleTypeDef;
typedef struct { unsigned Channel, Rank, SamplingTime, SingleDiff, OffsetNumber; } ADC_ChannelConfTypeDef;
static GPIO_TypeDef ga = {0xffff, 0}, gb = {0xffff, 0};
#define GPIOA (&ga)
#define GPIOB (&gb)
#define GPIO_PIN_2 (1U << 2)
#define GPIO_PIN_13 (1U << 13)
#define GPIO_PIN_14 (1U << 14)
#define GPIO_PIN_15 (1U << 15)
#define GPIO_PIN_RESET 0
#define GPIO_PIN_SET 1
#define HAL_OK 0
#define DAC_CHANNEL_1 1
#define DAC_ALIGN_12B_R 0
#define ADC_CHANNEL_3 3
#define ADC_CHANNEL_4 4
#define ADC_CHANNEL_12 12
#define ADC_CHANNEL_15 15
#define ADC_REGULAR_RANK_1 1
#define ADC_SAMPLETIME_640CYCLES_5 640
#define ADC_SINGLE_ENDED 0
#define ADC_OFFSET_NONE 0
static ADC_HandleTypeDef adc1, adc2, *last_adc;
static DAC_HandleTypeDef dac1;
volatile TB_Status tb_status;
static unsigned tick, stage, starts, reads, stops, written;
static unsigned codes[16] = {[3]=2000, [4]=1000, [12]=500, [15]=250};
static jmp_buf failed;
static uint32_t HAL_GetTick(void) { return tick; }
static int HAL_GPIO_ReadPin(GPIO_TypeDef *p, unsigned pin) { return !!(p->pins & pin); }
static void HAL_GPIO_WritePin(GPIO_TypeDef *p, unsigned pin, int on) {
  if (on) p->out |= pin; else p->out &= ~pin;
}
static void Fail(uint32_t code) { tb_status.fault=code; longjmp(failed, 1); }
static int HAL_DAC_SetValue(DAC_HandleTypeDef *h, unsigned ch, unsigned align, unsigned code) {
  (void)h; (void)ch; (void)align; written=code; return stage == 6;
}
static int HAL_ADC_Stop(ADC_HandleTypeDef *h) {
  (void)h; ++stops; return stage == 1 || (stage == 5 && stops > 1);
}
static int HAL_ADC_ConfigChannel(ADC_HandleTypeDef *h, ADC_ChannelConfTypeDef *cfg) {
  h->channel=cfg->Channel; last_adc=h; return stage == 2;
}
static int HAL_ADC_Start(ADC_HandleTypeDef *h) { (void)h; ++starts; return stage == 3; }
static int HAL_ADC_PollForConversion(ADC_HandleTypeDef *h, unsigned timeout) {
  (void)h; assert(timeout == 10); return stage == 4;
}
static unsigned HAL_ADC_GetValue(ADC_HandleTypeDef *h) {
  ++reads;
  /* First conversion is deliberately contaminated by the previous channel. */
  return reads == 1 ? 4095U : codes[h->channel];
}
static void near(float a, float b) { assert(fabsf(a-b) < 0.00001f); }
"""

cases = r"""
int main(void) {
  const float levels[6]={0.5f,1.0f,1.5f,2.0f,2.5f,3.0f};
  const unsigned expected[6]={620,1241,1861,2482,3102,3723};
  for (unsigned i=0; i<6; ++i) {
    TB_DacWrite(levels[i]); assert(written == expected[i]);
    assert(tb_status.dac_code == expected[i]);
    near(tb_status.dac_v, written*3.3f/4095.0f);
  }
  assert(TB_DacCode(-1.0f) == 248);
  assert(TB_DacCode(99.0f) == 3847);
  assert(TB_DacCode(NAN) == 3723 && TB_DacCode(INFINITY) == 3723);
  stage=6;
  if (setjmp(failed) == 0) { TB_DacWrite(1.0f); assert(!"DAC error not detected"); }
  assert(tb_status.fault == 12 && tb_status.dac_code == 3723);
  stage=0;

  unsigned channels[4]={4,3,12,15};
  TB_AdcSample sample={0};
  for (unsigned i=0; i<4; ++i) {
    starts=reads=stops=0;
    assert(TB_AdcRead(i,&sample) && sample.valid);
    assert(last_adc == (i<2 ? &adc2 : &adc1));
    assert(last_adc->channel == channels[i]);
    assert(starts == 17 && reads == 17 && stops == 2);
    assert(sample.code == codes[channels[i]]);
    near(sample.pin_v, codes[channels[i]]*3.3f/4095.0f);
  }
  codes[15]=0; reads=stops=0;
  assert(TB_AdcRead(3,&sample) && sample.valid && sample.code == 0);
  for (stage=1; stage<=5; ++stage) {
    sample.valid=true; sample.code=123; sample.pin_v=1.0f;
    reads=stops=0;
    assert(!TB_AdcRead(3,&sample) && !sample.valid);
    assert(sample.code == 123); near(sample.pin_v,1.0f);
  }
  stage=0;
  assert(!TB_AdcRead(4,&sample) && !sample.valid);
  assert(!TB_AdcRead(0,NULL));

  /* Bounce rejected, exactly one event on press, no repeat while held. */
  tick=100; assert(TB_KeysPoll() == 0);
  gb.pins &= ~GPIO_PIN_15; assert(TB_KeysPoll() == 0);
  tick+=10; gb.pins |= GPIO_PIN_15; assert(TB_KeysPoll() == 0);
  tick+=5; gb.pins &= ~GPIO_PIN_15; assert(TB_KeysPoll() == 0);
  tick+=19; assert(TB_KeysPoll() == 0);
  tick++; assert(TB_KeysPoll() == TB_K1 && tb_status.keys_down == TB_K1);
  tick+=5000; assert(TB_KeysPoll() == 0);
  gb.pins |= GPIO_PIN_15; assert(TB_KeysPoll() == 0);
  tick+=20; assert(TB_KeysPoll() == 0 && tb_status.keys_down == 0);
  tick=UINT32_MAX-10; gb.pins &= ~(GPIO_PIN_14|GPIO_PIN_13);
  assert(TB_KeysPoll() == 0);
  tick+=19; assert(TB_KeysPoll() == 0);
  tick++; assert(TB_KeysPoll() == (TB_K2|TB_K3));
  assert(tb_status.keys_raw == (TB_K2|TB_K3));
  tick+=1000; assert(TB_KeysPoll() == 0 && tb_status.heartbeat == tick);
  TB_Buzzer(true); assert(ga.out == GPIO_PIN_2);
  TB_Buzzer(false); assert(ga.out == 0);
  puts("module logic: DAC limits/errors, ADC mapping/discard/average/errors, keys/wrap passed");
}
"""

with tempfile.TemporaryDirectory(prefix="yld-module-tests-") as directory:
    path = pathlib.Path(directory)
    test_c = path / "module_test.c"
    test_c.write_text(header + stub + "\n".join(function(name) for name in (
        "TB_KeysPoll", "TB_Buzzer", "TB_DacCode", "TB_DacWrite", "TB_AdcRead"
    )) + cases)
    executable = path / "module_test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(test_c),
                    "-lm", "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
