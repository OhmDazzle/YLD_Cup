/**
  ******************************************************************************
  * @file    test_calib.c
  * @brief   采样标定模式 (TEST 文件夹) 实现.
  *
  *  上电直接进入 (或按住 K3 临时进入), 实时显示 4 个采样通道的原始值:
  *    CALIB MODE (RAW)
  *    VIN  xxxx +x.xxxxV     <- 标签 | 12 位 ADC 码值 | 引脚电压(16 次均值)
  *    VOUT xxxx +x.xxxxV
  *    IIN  xxxx +x.xxxxV
  *    IOUT xxxx +x.xxxxV
  *    OUT OFF  SET 8.0V      <- 输出通断 / 开环 DAC 目标
  *    K3:OUT K1/K2:SET
  *
  *  用仪器提供已知的输入/输出电压电流, 对照屏上原始值即可反推系数:
  *    电压通道: SCALE = 台表读数 / (raw - 零点)          -> 填 VIN/VOUT_ADC_SCALE
  *    电流通道: SCALE = 台表读数 / (raw - 零点); R = 1/(SCALE × 增益)
  *                                                      -> 填 IIN/IOUT_SHUNT_OHM
  *  完整步骤与算例见 TEST/README.md.
  ******************************************************************************
  */

#include <stddef.h>
#include "main.h"
#include "AD5662.h"
#include "OLED.h"
#include "test_calib.h"

/* ---- 本文件独立的小配置 (与主程序解耦, 改了不影响正常固件) ---- */
#define CAL_SAMPLING_TIME    ADC_SAMPLETIME_55CYCLES_5
#define CAL_OVERSAMPLING     16U    /* 标定用 16 次均值, 读数更稳 */
#define CAL_TIMEOUT_MS       10U
#define CAL_LOOP_MS          50U    /* 屏幕刷新间隔 */
#define CAL_DEBOUNCE_MS      20U

#define CAL_VOUT_DAC_A       16.4f  /* 与 main.c 的 VOUT_DAC_A/B 保持一致 (实测后可同步修正) */
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

/* ---- 内部工具 ---- */

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

/* 目标电压 -> DAC 输出电压 (与 main.c 的传递关系一致) */
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

/* ---- 对外接口 ---- */

void TestCalib_Run(ADC_HandleTypeDef *hadc_v, ADC_HandleTypeDef *hadc_i, AD5662_t *dac)
{
  uint16_t code_vin, code_vout, code_iin, code_iout;
  float vin, vout, iin, iout;
  float target = CAL_TARGET_DEFAULT;
  uint8_t out_on = 0U;

  /* 进入时保证输出关断 (B10 低), 蜂鸣器 (A2) 也先关掉 */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_RESET);

  while (1)
  {
    /* 1. 采 4 个通道 (电压组 -> 电流组) */
    vin  = Cal_ReadChannel(hadc_v, ADC_CHANNEL_7, &code_vin);   /* PA7 = INA_V_IN  */
    vout = Cal_ReadChannel(hadc_v, ADC_CHANNEL_6, &code_vout);  /* PA6 = INA_V_OUT */
    iin  = Cal_ReadChannel(hadc_i, ADC_CHANNEL_9, &code_iin);   /* PB1 = 输入电流  */
    iout = Cal_ReadChannel(hadc_i, ADC_CHANNEL_8, &code_iout);  /* PB0 = 输出电流  */

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
        (void)AD5662_SetVoltage(dac, Cal_TargetToDac(target)); /* 先预置 DAC 再合闸 */
      }
      HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, (out_on != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
    else if (Cal_KeyPressed(GPIOA, GPIO_PIN_8) != 0U)  /* K1: 目标 +1V */
    {
      target = Cal_Clamp(target + CAL_TARGET_STEP, CAL_TARGET_MIN, CAL_TARGET_MAX);
      if (out_on != 0U)
      {
        (void)AD5662_SetVoltage(dac, Cal_TargetToDac(target));
      }
    }
    else if (Cal_KeyPressed(GPIOB, GPIO_PIN_11) != 0U) /* K2: 目标 -1V */
    {
      target = Cal_Clamp(target - CAL_TARGET_STEP, CAL_TARGET_MIN, CAL_TARGET_MAX);
      if (out_on != 0U)
      {
        (void)AD5662_SetVoltage(dac, Cal_TargetToDac(target));
      }
    }
    else
    {
      /* 无按键 */
    }

    HAL_Delay(CAL_LOOP_MS);
  }
}
