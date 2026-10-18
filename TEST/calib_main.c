/**
  ******************************************************************************
  * @file    calib_main.c
  * @brief   独立采样标定固件 (TEST/): 单独编译、单独烧录, 与主固件互不影响.
  *
  *  本文件自成体系: 自带 main()、时钟与全部外设初始化, 不依赖 Core/Src/main.c,
  *  只复用板级文件(启动/中断/MSP)和自建驱动(OLED / AD5662).
  *  构建目标: YLD_Cup_Calib (见 TEST/CMakeLists.txt 与 TEST/README.md).
  *
  *  界面 (6x8 字体):
  *    CALIB MODE (RAW)
  *    VIN  xxxx +x.xxxxV     <- 标签 | 12 位 ADC 码值 | 引脚电压(16 次均值)
  *    VOUT xxxx +x.xxxxV
  *    IIN  xxxx +x.xxxxV
  *    IOUT xxxx +x.xxxxV
  *    OUT OFF  SET 8.0V      <- 输出通断 / 开环 DAC 目标
  *    K3:OUT K1/K2:SET
  *
  *  按键: K3(B9) 输出 MOS 开/关; K1(A8)/K2(B11) 目标 ±1V (5~15V).
  *  用仪器提供已知的输入/输出电压电流, 对照屏上原始值即可反推系数,
  *  测量方法与计算公式见 TEST/README.md.
  ******************************************************************************
  */

#include <stddef.h>
#include "stm32f1xx_hal.h"
#include "AD5662.h"
#include "OLED.h"

/* ---- 本固件的可调参数 ---- */
#define CAL_SAMPLING_TIME    ADC_SAMPLETIME_55CYCLES_5
#define CAL_OVERSAMPLING     16U    /* 标定用 16 次均值, 读数更稳 */
#define CAL_TIMEOUT_MS       10U
#define CAL_LOOP_MS          50U    /* 屏幕刷新间隔 */
#define CAL_DEBOUNCE_MS      20U

#define CAL_VOUT_DAC_A       16.4f  /* 与主固件 VOUT_DAC_A/B 保持一致 (实测后可同步修正) */
#define CAL_VOUT_DAC_B       4.0f
#define CAL_DAC_VMAX         3.3f
#define CAL_TARGET_DEFAULT   8.0f
#define CAL_TARGET_MIN       5.0f
#define CAL_TARGET_MAX       15.0f
#define CAL_TARGET_STEP      1.0f

/* 屏幕列位置 (6x8 字体) */
#define CAL_CODE_X           24U
#define CAL_VOLT_X           54U
#define CAL_UNIT_X           102U

/* ---- 外设句柄 ---- */
static ADC_HandleTypeDef hadc1;
static ADC_HandleTypeDef hadc2;
static SPI_HandleTypeDef hspi2;
static AD5662_t dac;

/* ---- 内部函数声明 ---- */
static void Error_Handler(void);
static void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_ADC2_Init(void);
static void MX_SPI2_Init(void);
static float Cal_Clamp(float v, float lo, float hi);
static float Cal_TargetToDac(float vout);
static float Cal_ReadChannel(ADC_HandleTypeDef *hadc, uint32_t channel, uint16_t *code_out);
static uint8_t Cal_KeyPressed(GPIO_TypeDef *port, uint16_t pin);
static void Cal_ShowRow(int16_t y, char *name, uint16_t code, float volt);

/* ============================ 主函数 ============================ */

int main(void)
{
  uint16_t code_vin, code_vout, code_iin, code_iout;
  float vin, vout, iin, iout;
  float target = CAL_TARGET_DEFAULT;
  uint8_t out_on = 0U;

  HAL_Init();
  SystemClock_Config();

  MX_GPIO_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();
  MX_SPI2_Init();

  /* OLED + AD5662 初始化; 输出与蜂鸣器先关断 */
  OLED_Init();
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);  /* 输出 MOS 关 */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2,  GPIO_PIN_RESET);  /* 蜂鸣器关 */
  AD5662_Init(&dac, &hspi2, GPIOB, GPIO_PIN_12, 3.3f);
  (void)AD5662_SetVoltage(&dac, Cal_TargetToDac(CAL_TARGET_DEFAULT)); /* 先按目标预置 DAC */

  if (HAL_ADCEx_Calibration_Start(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_ADCEx_Calibration_Start(&hadc2) != HAL_OK)
  {
    Error_Handler();
  }

  /* ---- 标定界面: 不返回 ---- */
  while (1)
  {
    /* 1. 采 4 个通道 (电压组 -> 电流组) */
    vin  = Cal_ReadChannel(&hadc1, ADC_CHANNEL_7, &code_vin);   /* PA7 = INA_V_IN  */
    vout = Cal_ReadChannel(&hadc1, ADC_CHANNEL_6, &code_vout);  /* PA6 = INA_V_OUT */
    iin  = Cal_ReadChannel(&hadc2, ADC_CHANNEL_9, &code_iin);   /* PB1 = 输入电流  */
    iout = Cal_ReadChannel(&hadc2, ADC_CHANNEL_8, &code_iout);  /* PB0 = 输出电流  */

    /* 2. 刷屏 */
    OLED_Clear();
    OLED_ShowString(0, 0, "CALIB MODE (RAW)", OLED_6X8);
    Cal_ShowRow(8,  "VIN",  code_vin,  vin);
    Cal_ShowRow(16, "VOUT", code_vout, vout);
    Cal_ShowRow(24, "IIN",  code_iin,  iin);
    Cal_ShowRow(32, "IOUT", code_iout, iout);

    OLED_ShowString(0, 40, "OUT", OLED_6X8);
    OLED_ShowString(24, 40, (out_on != 0U) ? "ON " : "OFF", OLED_6X8);
    OLED_ShowString(60, 40, "SET", OLED_6X8);
    OLED_ShowFloatNum(84, 40, target, 1U, 1U, OLED_6X8);
    OLED_ShowChar(114, 40, 'V', OLED_6X8);

    OLED_ShowString(0, 48, "K3:OUT K1/K2:SET", OLED_6X8);
    OLED_ShowString(0, 56, "AVG=16 SAMPLES", OLED_6X8);
    OLED_Update();

    /* 3. 按键 (K3 优先): 输出开关 / 目标 ±1V */
    if (Cal_KeyPressed(GPIOB, GPIO_PIN_9) != 0U)       /* K3: 输出开/关 */
    {
      out_on = (out_on == 0U) ? 1U : 0U;
      if (out_on != 0U)
      {
        (void)AD5662_SetVoltage(&dac, Cal_TargetToDac(target)); /* 先预置 DAC 再合闸 */
      }
      HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, (out_on != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
    else if (Cal_KeyPressed(GPIOA, GPIO_PIN_8) != 0U)  /* K1: 目标 +1V */
    {
      target = Cal_Clamp(target + CAL_TARGET_STEP, CAL_TARGET_MIN, CAL_TARGET_MAX);
      if (out_on != 0U)
      {
        (void)AD5662_SetVoltage(&dac, Cal_TargetToDac(target));
      }
    }
    else if (Cal_KeyPressed(GPIOB, GPIO_PIN_11) != 0U) /* K2: 目标 -1V */
    {
      target = Cal_Clamp(target - CAL_TARGET_STEP, CAL_TARGET_MIN, CAL_TARGET_MAX);
      if (out_on != 0U)
      {
        (void)AD5662_SetVoltage(&dac, Cal_TargetToDac(target));
      }
    }
    else
    {
      /* 无按键 */
    }

    HAL_Delay(CAL_LOOP_MS);
  }
}

/* ============================ 内部实现 ============================ */

static float Cal_Clamp(float v, float lo, float hi)
{
  if (v < lo)
  {
    return lo;
  }
  if (v > hi)
  {
    return hi;
  }
  return v;
}

/* 目标电压 -> DAC 输出电压 (与主固件的传递关系一致) */
static float Cal_TargetToDac(float vout)
{
  return Cal_Clamp((CAL_VOUT_DAC_A - vout) / CAL_VOUT_DAC_B, 0.0f, CAL_DAC_VMAX);
}

/* 读一个通道: 16 次过采样取均值, 并回传 12 位码值 (失败回 0) */
static float Cal_ReadChannel(ADC_HandleTypeDef *hadc, uint32_t channel, uint16_t *code_out)
{
  ADC_ChannelConfTypeDef sConfig = {0};
  uint32_t sum = 0U;
  uint32_t i;

  HAL_ADC_Stop(hadc);
  sConfig.Channel = channel;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = CAL_SAMPLING_TIME;
  if (HAL_ADC_ConfigChannel(hadc, &sConfig) != HAL_OK)
  {
    if (code_out != NULL)
    {
      *code_out = 0U;
    }
    return 0.0f;
  }

  for (i = 0U; i < CAL_OVERSAMPLING; i++)
  {
    if (HAL_ADC_Start(hadc) != HAL_OK)
    {
      HAL_ADC_Stop(hadc);
      if (code_out != NULL)
      {
        *code_out = 0U;
      }
      return 0.0f;
    }
    if (HAL_ADC_PollForConversion(hadc, CAL_TIMEOUT_MS) != HAL_OK)
    {
      HAL_ADC_Stop(hadc);
      if (code_out != NULL)
      {
        *code_out = 0U;
      }
      return 0.0f;
    }
    sum += HAL_ADC_GetValue(hadc);
  }
  HAL_ADC_Stop(hadc);

  if (code_out != NULL)
  {
    *code_out = (uint16_t)(sum / CAL_OVERSAMPLING);
  }
  return ((float)sum / (float)CAL_OVERSAMPLING) * 3.3f / 4095.0f;
}

/* 简单按键: 按下(低)且消抖确认后返回 1, 并等松手 (标定界面够用, 不做连发) */
static uint8_t Cal_KeyPressed(GPIO_TypeDef *port, uint16_t pin)
{
  if (HAL_GPIO_ReadPin(port, pin) != GPIO_PIN_RESET)
  {
    return 0U;
  }
  HAL_Delay(CAL_DEBOUNCE_MS);
  if (HAL_GPIO_ReadPin(port, pin) != GPIO_PIN_RESET)
  {
    return 0U;
  }
  while (HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_RESET)
  {
    /* 等松手, 避免误连发 */
  }
  return 1U;
}

/* 画一行: 标签 | 码值 | +x.xxxxV */
static void Cal_ShowRow(int16_t y, char *name, uint16_t code, float volt)
{
  OLED_ShowString(0, y, name, OLED_6X8);
  OLED_ShowNum(CAL_CODE_X, y, code, 4U, OLED_6X8);
  OLED_ShowFloatNum(CAL_VOLT_X, y, volt, 1U, 4U, OLED_6X8);
  OLED_ShowChar(CAL_UNIT_X, y, 'V', OLED_6X8);
}

/**
  * @brief System Clock Configuration (与 CubeMX 生成的一致: HSE 8MHz x9 = 72MHz, ADC /6)
  */
static void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }

  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV6;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief GPIO 初始化 (只配标定用到的脚: PA2 蜂鸣器, PB10 输出 MOS, PB12 SYNC,
  *        A8/B11/B9 按键; PA4/PA5 由 OLED_Init 自行配置)
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* 输出初始电平: 蜂鸣器关(A2), 输出 MOS 关(B10), SYNC 空闲高(B12) */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_SET);

  /* PA2: 蜂鸣器 MOS; PB10/PB12: 输出 MOS / SYNC */
  GPIO_InitStruct.Pin = GPIO_PIN_2;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = GPIO_PIN_10|GPIO_PIN_12;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* 按键: A8 / B11 / B9 上拉输入 (按下=低) */
  GPIO_InitStruct.Pin = GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = GPIO_PIN_11|GPIO_PIN_9;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}

/**
  * @brief ADC1 初始化 (电压通道: PA6/PA7; 参数与主固件一致)
  */
static void MX_ADC1_Init(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};

  hadc1.Instance = ADC1;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  sConfig.Channel = ADC_CHANNEL_6;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_1CYCLE_5;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC2 初始化 (电流通道: PB0/PB1; 参数与主固件一致)
  */
static void MX_ADC2_Init(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};

  hadc2.Instance = ADC2;
  hadc2.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc2.Init.ContinuousConvMode = DISABLE;
  hadc2.Init.DiscontinuousConvMode = DISABLE;
  hadc2.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc2.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc2.Init.NbrOfConversion = 1;
  if (HAL_ADC_Init(&hadc2) != HAL_OK)
  {
    Error_Handler();
  }

  sConfig.Channel = ADC_CHANNEL_8;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_1CYCLE_5;
  if (HAL_ADC_ConfigChannel(&hadc2, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief SPI2 初始化 (AD5662: 参数与主固件一致)
  */
static void MX_SPI2_Init(void)
{
  hspi2.Instance = SPI2;
  hspi2.Init.Mode = SPI_MODE_MASTER;
  hspi2.Init.Direction = SPI_DIRECTION_2LINES;
  hspi2.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi2.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi2.Init.CLKPhase = SPI_PHASE_2EDGE;
  hspi2.Init.NSS = SPI_NSS_SOFT;
  hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
  hspi2.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi2.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi2.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief 出错处理: 关中断并停住
  */
static void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}
