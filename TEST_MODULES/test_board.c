/* 共用板级初始化，不链接业务 main.c；本文件始终不打开 PA1。
 * 模块自检保持关断，独立整机调参固件由调用方显式管理 PA1。 */
#include <math.h>
#include "test_board.h"

volatile TB_Status tb_status;
static ADC_HandleTypeDef adc1, adc2;
static DAC_HandleTypeDef dac1;

static void Fail(uint32_t code)
{
  tb_status.fault = code;
  __HAL_RCC_GPIOA_CLK_ENABLE();
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1 | GPIO_PIN_2, GPIO_PIN_RESET);
  __disable_irq();
  while (1) { /* 在 Watch 查看 tb_status.fault，见 README 的故障码。 */ }
}

void Error_Handler(void) { Fail(1U); }

static void ClockInit(void)
{
  RCC_OscInitTypeDef osc = {0};
  RCC_ClkInitTypeDef clk = {0};
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST) != HAL_OK)
    Fail(2U);
  osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  osc.HSEState = RCC_HSE_ON;
  osc.PLL.PLLState = RCC_PLL_ON;
  osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  osc.PLL.PLLM = RCC_PLLM_DIV2;
  osc.PLL.PLLN = 85;
  osc.PLL.PLLP = RCC_PLLP_DIV2;
  osc.PLL.PLLQ = RCC_PLLQ_DIV2;
  osc.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) Fail(3U);
  clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clk.APB1CLKDivider = RCC_HCLK_DIV1;
  clk.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4) != HAL_OK) Fail(4U);
}

void TB_Init(void)
{
  GPIO_InitTypeDef gpio = {0};
  HAL_Init();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  /* 先关 Q3 和蜂鸣器，再切换时钟及初始化其他模块。 */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1 | GPIO_PIN_2, GPIO_PIN_RESET);
  gpio.Pin = GPIO_PIN_1 | GPIO_PIN_2;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &gpio);
  gpio.Pin = GPIO_PIN_4 | GPIO_PIN_6 | GPIO_PIN_7;
  gpio.Mode = GPIO_MODE_ANALOG;
  HAL_GPIO_Init(GPIOA, &gpio);
  gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1;
  HAL_GPIO_Init(GPIOB, &gpio);
  gpio.Pin = GPIO_PIN_15 | GPIO_PIN_14 | GPIO_PIN_13;
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &gpio);
  ClockInit();
}

uint8_t TB_KeysPoll(void)
{
  static const uint16_t pins[3] = {GPIO_PIN_15, GPIO_PIN_14, GPIO_PIN_13};
  static uint8_t previous[3], stable[3];
  static uint32_t changed[3];
  uint8_t events = 0U, raw_mask = 0U, down_mask = 0U;
  uint32_t now = HAL_GetTick();
  for (unsigned i = 0; i < 3U; ++i) {
    uint8_t down = HAL_GPIO_ReadPin(GPIOB, pins[i]) == GPIO_PIN_RESET;
    uint8_t bit = (uint8_t)(1U << i);
    if (down) raw_mask |= bit;
    if (down != previous[i]) { previous[i] = down; changed[i] = now; }
    if (down != stable[i] && (uint32_t)(now - changed[i]) >= 20U) {
      stable[i] = down;
      if (down) events |= bit;
    }
    if (stable[i]) down_mask |= bit;
  }
  tb_status.keys_raw = raw_mask;
  tb_status.keys_down = down_mask;
  tb_status.heartbeat = now;
  return events;
}

void TB_Buzzer(bool on)
{
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

uint16_t TB_DacCode(float volts)
{
  /* 非有限值回退高 DAC 电压（按当前 FB 极性对应较低输出）。 */
  if (!isfinite(volts)) volts = 3.0f;
  if (volts < 0.2f) volts = 0.2f;
  if (volts > 3.1f) volts = 3.1f;
  return (uint16_t)(volts * 4095.0f / TB_VREF + 0.5f);
}

void TB_DacWrite(float volts)
{
  uint16_t code = TB_DacCode(volts);
  if (HAL_DAC_SetValue(&dac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, code) != HAL_OK)
    Fail(12U);
  tb_status.dac_code = code;
  tb_status.dac_v = (float)code * TB_VREF / 4095.0f;
}

void TB_DacInit(float volts)
{
  DAC_ChannelConfTypeDef cfg = {0};
  dac1.Instance = DAC1;
  if (HAL_DAC_Init(&dac1) != HAL_OK) Fail(10U);
  cfg.DAC_HighFrequency = DAC_HIGH_FREQUENCY_INTERFACE_MODE_AUTOMATIC;
  cfg.DAC_DMADoubleDataMode = DISABLE;
  cfg.DAC_SignedFormat = DISABLE;
  cfg.DAC_SampleAndHold = DAC_SAMPLEANDHOLD_DISABLE;
  cfg.DAC_Trigger = DAC_TRIGGER_NONE;
  cfg.DAC_Trigger2 = DAC_TRIGGER_NONE;
  cfg.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
  cfg.DAC_ConnectOnChipPeripheral = DAC_CHIPCONNECT_EXTERNAL;
  cfg.DAC_UserTrimming = DAC_TRIMMING_FACTORY;
  if (HAL_DAC_ConfigChannel(&dac1, &cfg, DAC_CHANNEL_1) != HAL_OK) Fail(11U);
  TB_DacWrite(volts); /* 在使能输出前预装电压，避免主动输出 0V 档位。 */
  if (HAL_DAC_Start(&dac1, DAC_CHANNEL_1) != HAL_OK) Fail(13U);
}

static void AdcInitOne(ADC_HandleTypeDef *adc, ADC_TypeDef *instance)
{
  adc->Instance = instance;
  adc->Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV8;
  adc->Init.Resolution = ADC_RESOLUTION_12B;
  adc->Init.DataAlign = ADC_DATAALIGN_RIGHT;
  adc->Init.GainCompensation = 0U;
  adc->Init.ScanConvMode = ADC_SCAN_DISABLE;
  adc->Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  adc->Init.LowPowerAutoWait = DISABLE;
  adc->Init.ContinuousConvMode = DISABLE;
  adc->Init.NbrOfConversion = 1U;
  adc->Init.DiscontinuousConvMode = DISABLE;
  adc->Init.ExternalTrigConv = ADC_SOFTWARE_START;
  adc->Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  adc->Init.DMAContinuousRequests = DISABLE;
  adc->Init.Overrun = ADC_OVR_DATA_PRESERVED;
  adc->Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(adc) != HAL_OK) Fail(instance == ADC1 ? 20U : 21U);
}

void TB_AdcInit(void)
{
  ADC_MultiModeTypeDef mode = {0};
  AdcInitOne(&adc1, ADC1);
  mode.Mode = ADC_MODE_INDEPENDENT;
  if (HAL_ADCEx_MultiModeConfigChannel(&adc1, &mode) != HAL_OK) Fail(22U);
  AdcInitOne(&adc2, ADC2);
  if (HAL_ADCEx_Calibration_Start(&adc1, ADC_SINGLE_ENDED) != HAL_OK) Fail(23U);
  if (HAL_ADCEx_Calibration_Start(&adc2, ADC_SINGLE_ENDED) != HAL_OK) Fail(24U);
}

bool TB_AdcRead(unsigned index, TB_AdcSample *sample)
{
  static const uint32_t channels[4] = {
    ADC_CHANNEL_4, ADC_CHANNEL_3, ADC_CHANNEL_12, ADC_CHANNEL_15
  };
  ADC_ChannelConfTypeDef cfg = {0};
  ADC_HandleTypeDef *adc = index < 2U ? &adc2 : &adc1;
  uint32_t sum = 0U;
  if (sample == NULL) return false;
  sample->valid = false; /* 失败保留旧数值，但必须用 valid 区分旧值/真实零。 */
  if (index >= 4U || HAL_ADC_Stop(adc) != HAL_OK) return false;
  cfg.Channel = channels[index];
  cfg.Rank = ADC_REGULAR_RANK_1;
  cfg.SamplingTime = ADC_SAMPLETIME_640CYCLES_5;
  cfg.SingleDiff = ADC_SINGLE_ENDED;
  cfg.OffsetNumber = ADC_OFFSET_NONE;
  if (HAL_ADC_ConfigChannel(adc, &cfg) != HAL_OK) return false;
  for (unsigned i = 0U; i <= TB_ADC_SAMPLES; ++i) {
    if (HAL_ADC_Start(adc) != HAL_OK ||
        HAL_ADC_PollForConversion(adc, 10U) != HAL_OK) {
      (void)HAL_ADC_Stop(adc);
      return false;
    }
    uint32_t code = HAL_ADC_GetValue(adc);
    if (i != 0U) sum += code; /* 通道切换后弃首样，再平均 TB_ADC_SAMPLES 次。 */
  }
  if (HAL_ADC_Stop(adc) != HAL_OK) return false;
  sample->code = (uint16_t)((sum + TB_ADC_SAMPLES / 2U) / TB_ADC_SAMPLES);
  sample->pin_v = (float)sum * TB_VREF / (4095.0f * TB_ADC_SAMPLES);
  sample->valid = true;
  return true;
}
