#!/usr/bin/env python3
"""Run main.c USER CODE on the host with simulated HAL, without STM32 hardware."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
source = (ROOT / "Core/Src/main.c").read_text()


def user_block(name):
    return source.split(f"/* USER CODE BEGIN {name} */", 1)[1].split(
        f"/* USER CODE END {name} */", 1
    )[0]


stub = r"""
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "measurement_calibration.h"
typedef struct { uint16_t in, out; } GPIO_TypeDef;
static GPIO_TypeDef ga = {0xffff, 0}, gb = {0xffff, 0};
#define GPIOA (&ga)
#define GPIOB (&gb)
#define GPIO_PIN_1 (1U << 1)
#define GPIO_PIN_2 (1U << 2)
#define GPIO_PIN_13 (1U << 13)
#define GPIO_PIN_14 (1U << 14)
#define GPIO_PIN_15 (1U << 15)
#define GPIO_PIN_SET 1
#define GPIO_PIN_RESET 0
typedef struct { uint32_t Channel, Rank, SamplingTime, SingleDiff, OffsetNumber, Offset; } ADC_ChannelConfTypeDef;
typedef struct { uint32_t channel; } ADC_HandleTypeDef;
typedef struct { int unused; } DAC_HandleTypeDef;
typedef int GPIO_PinState;
#define ADC_SAMPLETIME_640CYCLES_5 640
#define ADC_REGULAR_RANK_1 1
#define ADC_SINGLE_ENDED 0
#define ADC_OFFSET_NONE 0
#define ADC_CHANNEL_3 3
#define ADC_CHANNEL_4 4
#define ADC_CHANNEL_12 12
#define ADC_CHANNEL_15 15
#define DAC_CHANNEL_1 1
#define DAC_ALIGN_12B_R 0
#define HAL_OK 0
#define OLED_6X8 6
#define OLED_Clear(...) ((void)0)
#define OLED_ShowString(...) ((void)0)
#define OLED_ShowFloatNum(...) ((void)0)
#define OLED_ShowChar(...) ((void)0)
#define OLED_Update(...) ((void)0)
static ADC_HandleTypeDef hadc1, hadc2;
static DAC_HandleTypeDef hdac1;
static uint32_t tick, dacCode, starts, polls, reads, stage;
static uint32_t codes[16] = {[3]=1418, [4]=2836, [12]=1241, [15]=2482};
static uint32_t HAL_GetTick(void) { return tick; }
static void HAL_Delay(uint32_t ms) { tick += ms; }
static int HAL_GPIO_ReadPin(GPIO_TypeDef *p, uint16_t pin) { return !!(p->in & pin); }
static void HAL_GPIO_WritePin(GPIO_TypeDef *p, uint16_t pin, int on) {
  if (on) p->out |= pin; else p->out &= (uint16_t)~pin;
}
static int HAL_ADC_Stop(ADC_HandleTypeDef *h) { (void)h; return stage == 1; }
static int HAL_ADC_ConfigChannel(ADC_HandleTypeDef *h, ADC_ChannelConfTypeDef *c) {
  h->channel=c->Channel; return stage == 2;
}
static int HAL_ADC_Start(ADC_HandleTypeDef *h) { (void)h; starts++; return stage == 3; }
static int HAL_ADC_PollForConversion(ADC_HandleTypeDef *h, uint32_t ms) {
  (void)h; (void)ms; polls++; return stage == 4;
}
static uint32_t HAL_ADC_GetValue(ADC_HandleTypeDef *h) { reads++; return codes[h->channel]; }
static int HAL_DAC_SetValue(DAC_HandleTypeDef *h, uint32_t ch, uint32_t align, uint32_t code) {
  (void)h; (void)ch; (void)align; dacCode=code; return 0;
}
void Error_Handler(void) { abort(); }
static void near(float a, float b, float epsilon) { assert(fabsf(a-b) < epsilon); }
"""

cases = r"""
static void sample_vin(float vin) {
  codes[4] = (uint32_t)(vin / VIN_ADC_SCALE * ADC_FULL_SCALE / ADC_VREF + 0.5f);
  assert(App_SampleVoltageGroup());
  App_ProtectionCheck();
}

int main(void) {
  /* Real hardware transfer functions and calibration, including signed intercept. */
  near(App_TargetToDac(5.0f), 2.6556f, 0.0001f);
  near(App_TargetToDac(8.0f), 1.9956f, 0.0001f);
  near(App_TargetToDac(15.0f), 0.4556f, 0.0001f);
  assert(App_TargetToDac(5.0f) > App_TargetToDac(15.0f));
  near(Cal_Iin(0.0f), 0.0f, 0.00001f);
  near(Cal_Iout(0.0f), 0.0f, 0.00001f);
  App_DacSetVoltage(NAN); assert(dacCode == 3847);
  App_DacSetVoltage(INFINITY); assert(dacCode == 3847);
  App_DacSetVoltage(-1.0f); assert(dacCode == 248);
  App_DacSetVoltage(9.0f); assert(dacCode == 3847);

  /* A true 0V conversion succeeds; each HAL failure leaves the output untouched. */
  float v = -1.0f;
  codes[15]=0; starts=polls=reads=0;
  assert(ADC_ReadVoltage(&hadc1, ADC_CHANNEL_15, &v));
  near(v, 0.0f, 0.00001f);
  assert(starts == 5 && polls == 5 && reads == 4);
  for (stage=1; stage<=4; stage++) {
    v=42.0f;
    assert(!ADC_ReadVoltage(&hadc1, ADC_CHANNEL_15, &v));
    near(v,42.0f,0.00001f);
  }
  stage=0;

  /* Fresh measurements before closing the switch, current scaling, ADC fault latch. */
  App_SetState(APP_ST_CV);
  assert(appState == APP_ST_CV && (ga.out & GPIO_PIN_1));
  near(measIin, Cal_Iin(1241.0f * ADC_VREF / ADC_FULL_SCALE), 0.00001f);
  codes[15]=2482;
  assert(App_SampleCurrentGroup());
  near(measIout, Cal_Iout(2482.0f * ADC_VREF / ADC_FULL_SCALE), 0.00001f);
  near(measVout, Cal_Vout(measVoutNominal, measIout), 0.00001f);
  float lastVout = measVout;
  assert(App_SampleCurrentGroup()); near(measVout, lastVout, 0.00001f);
  codes[15]=621; assert(App_SampleCurrentGroup());
  assert(measVout > lastVout); /* New current refreshes cached terminal estimate. */
  assert(App_SampleVoltageGroup());
  near(measVout, Cal_Vout(measVoutNominal, measIout), 0.00001f);
  stage=4; assert(!App_SampleCurrentGroup());
  assert(appState == APP_ST_PROT_ADC && !(ga.out & GPIO_PIN_1));
  assert(ga.out & GPIO_PIN_2);
  App_HandleKeyEvent(KEY_IDX_FUNC, KEY_EV_SHORT);
  assert(appState == APP_ST_PROT_ADC);
  stage=0; App_HandleKeyEvent(KEY_IDX_FUNC, KEY_EV_SHORT);
  assert(appState == APP_ST_STANDBY && !(ga.out & GPIO_PIN_1));

  /* UVP still prevents closing when the input is already below the threshold. */
  codes[4]=1773; App_SetState(APP_ST_CV);
  assert(appState == APP_ST_PROT_UVP && !(ga.out & GPIO_PIN_1));
  codes[4]=2836; assert(App_SampleVoltageGroup()); App_ProtectionCheck();
  assert(appState == APP_ST_CV && (ga.out & GPIO_PIN_1));

  /* CV and CC stay enabled until 3 seconds of sampled undervoltage, including
     when confirmation starts at tick zero during the OCP blanking window. */
  assert(UVP_CONFIRM_MS == 3000U);
  for (unsigned mode=0; mode<2; mode++) {
    tick=mode*10000U;
    codes[4]=2836; App_SetState(mode ? APP_ST_CC : APP_ST_CV);
    AppState_t running=appState;
    sample_vin(12.0f);
    assert(appState == running && (ga.out & GPIO_PIN_1));
    assert(!(ga.out & GPIO_PIN_2));
    tick+=2999U; sample_vin(12.0f);
    assert(appState == running && (ga.out & GPIO_PIN_1));
    tick++; App_ProtectionCheck(); /* No fresh sample must not confirm UVP. */
    assert(appState == running);
    sample_vin(12.0f);
    assert(appState == APP_ST_PROT_UVP && !(ga.out & GPIO_PIN_1));
    assert(ga.out & GPIO_PIN_2);
    sample_vin(12.9f);
    assert(appState == APP_ST_PROT_UVP);
    sample_vin(13.1f);
    assert(appState == running && (ga.out & GPIO_PIN_1));
    assert(!(ga.out & GPIO_PIN_2));
  }

  /* A normal sample above 12.5V resets confirmation even below 13V recovery. */
  tick=20000U; sample_vin(12.0f);
  tick+=2999U; sample_vin(12.6f);
  assert(appState == APP_ST_CC);
  tick+=2U; sample_vin(12.0f);
  assert(appState == APP_ST_CC);
  tick+=2999U; sample_vin(12.0f);
  assert(appState == APP_ST_CC);
  tick++; sample_vin(12.0f);
  assert(appState == APP_ST_PROT_UVP);

  /* Standby and automatic recovery must not carry an earlier timer forward. */
  sample_vin(16.0f);
  sample_vin(12.0f);
  tick+=2999U; sample_vin(12.0f);
  assert(appState == APP_ST_CC);
  App_SetState(APP_ST_STANDBY);
  codes[4]=2836; App_SetState(APP_ST_CV);
  sample_vin(12.0f);
  tick++; sample_vin(12.0f);
  assert(appState == APP_ST_CV);
  tick+=2999U; sample_vin(12.0f);
  assert(appState == APP_ST_PROT_UVP);

  /* The 3-second elapsed-time check also works across the 32-bit tick wrap. */
  tick=UINT32_MAX-1000U;
  codes[4]=2836; App_SetState(APP_ST_CV);
  sample_vin(12.0f);
  tick+=2999U; sample_vin(12.0f);
  assert(appState == APP_ST_CV);
  tick++; sample_vin(12.0f);
  assert(appState == APP_ST_PROT_UVP && !(ga.out & GPIO_PIN_1));

  /* ADC failure still shuts down immediately while UVP is being confirmed. */
  codes[4]=2836; App_SetState(APP_ST_CV);
  sample_vin(12.0f);
  stage=4; assert(!App_SampleVoltageGroup());
  assert(appState == APP_ST_PROT_ADC && !(ga.out & GPIO_PIN_1));
  stage=0;

  /* UVP confirmation does not delay an independent OCP trip. */
  codes[4]=2836; App_SetState(APP_ST_CV); tick=protBlankUntil;
  sample_vin(12.0f);
  measIout=2.4f; ioutFresh=1; App_ProtectionCheck();
  tick+=OCP_CONFIRM_MS; ioutFresh=1; App_ProtectionCheck();
  assert(appState == APP_ST_PROT_OCP && !(ga.out & GPIO_PIN_1));

  /* OCP counts new samples only and a normal sample resets the confirmation. */
  codes[4]=2836; App_SetState(APP_ST_CV); tick=protBlankUntil;
  measIout=2.4f; ioutFresh=1; App_ProtectionCheck();
  tick+=100; App_ProtectionCheck(); assert(appState == APP_ST_CV);
  measIout=2.0f; ioutFresh=1; App_ProtectionCheck(); assert(!ocpTracking);
  measIout=2.4f; ioutFresh=1; App_ProtectionCheck();
  tick+=49; ioutFresh=1; App_ProtectionCheck(); assert(appState == APP_ST_CV);
  tick++; ioutFresh=1; App_ProtectionCheck();
  assert(appState == APP_ST_PROT_OCP && !(ga.out & GPIO_PIN_1));

  /* PB13 short/long events use elapsed time, including tick wraparound. */
  assert(keys[0].port == GPIOB && keys[0].pin == GPIO_PIN_15);
  assert(keys[1].pin == GPIO_PIN_14 && keys[2].pin == GPIO_PIN_13);
  tick=UINT32_MAX-10; gb.in &= (uint16_t)~GPIO_PIN_13; App_ScanKeys();
  tick+=19; App_ScanKeys(); assert(keys[2].stable == 1);
  tick++; App_ScanKeys(); assert(keys[2].stable == 0);
  tick+=799; App_ScanKeys(); assert(keys[2].event == KEY_EV_NONE);
  tick++; App_ScanKeys(); assert(keys[2].event == KEY_EV_LONG);
  tick+=20; App_ScanKeys(); assert(keys[2].event == KEY_EV_NONE);
  gb.in |= GPIO_PIN_13; App_ScanKeys(); tick+=20; App_ScanKeys();
  assert(keys[2].event == KEY_EV_NONE);
  tick+=10; gb.in &= (uint16_t)~GPIO_PIN_13; App_ScanKeys();
  tick+=20; App_ScanKeys(); tick+=30; gb.in |= GPIO_PIN_13; App_ScanKeys();
  tick+=20; App_ScanKeys(); assert(keys[2].event == KEY_EV_SHORT);
  puts("PASS: calibration, DAC limits, ADC failures, state transitions, protection and keys");
  return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="yld-main-test-") as directory:
    cfile = pathlib.Path(directory) / "main_logic.c"
    binary = pathlib.Path(directory) / "main_logic"
    cfile.write_text(stub + "\n".join(user_block(n) for n in ("PTD", "PD", "PV", "PFP", "4")) + cases)
    subprocess.run(["cc", "-std=c11", "-I", str(ROOT / "Core/Inc"), str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
