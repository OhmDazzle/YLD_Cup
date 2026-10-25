/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "OLED.h"
#include "measurement_calibration.h"
#include <math.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* 应用状态机 (题目 1.1 / 1.2 / 2.1 + 恒流模式) */
typedef enum
{
  APP_ST_SELF_TEST = 0,   /* 开机自检 (蜂鸣 3 声) */
  APP_ST_STANDBY,         /* 待机: 无输出 */
  APP_ST_CV,              /* 恒压模式 */
  APP_ST_CC,              /* 恒流模式 */
  APP_ST_PROT_OCP,        /* 过流保护: 关输出 + 报警, 手动解除 */
  APP_ST_PROT_UVP,        /* 欠压保护: 关输出 + 报警, 自动恢复 */
  APP_ST_PROT_ADC,        /* 采样故障: 关输出 + 报警, 手动重试后回待机 */
} AppState_t;

/* 按键事件 */
typedef enum
{
  KEY_EV_NONE = 0,
  KEY_EV_SHORT,           /* 短按 (松手时产生) */
  KEY_EV_LONG,            /* 长按 (按住超过 KEY_LONG_MS, 只产生一次) */
} KeyEvent_t;

/* 按键对象 */
typedef struct
{
  GPIO_TypeDef *port;
  uint16_t      pin;
  uint8_t       stable;     /* 消抖后的稳定电平: 1=松开, 0=按下 */
  uint8_t       rawLast;    /* 上一次原始电平 */
  uint32_t      changedAt;  /* 原始电平改变的时刻 */
  uint32_t      pressedAt;  /* 消抖确认按下的时刻 */
  uint8_t       longFired;  /* 已产生过长按事件 */
  KeyEvent_t    event;      /* 本次扫描产生的事件 */
} Key_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* ============================================================================
 * 应用层运行参数集中在这里; 实测系数见 measurement_calibration.h
 * ==========================================================================*/

/* ---- ADC 基础参数 ---- */
/* ADC 参考电压: VREF+ 接 VDDA, 本板为 3.3V */
#define ADC_VREF             3.3f
/* 12 位 ADC 满量程值 */
#define ADC_FULL_SCALE       4095.0f
/* 2026-10-20 原理图: 60kΩ/10kΩ 分压, 节点对地 100nF.
   保留 640.5 周期与切换后弃样, ADC 时钟 170/8=21.25MHz;
   单次采样约 30us, 一个通道含弃样及 4 次均值至少约 150us. */
#define ADC_SAMPLING_TIME    ADC_SAMPLETIME_640CYCLES_5
/* 每个通道的过采样次数, 用于简单均值滤波 */
#define ADC_OVERSAMPLING     4U
/* 单次转换的等待超时, 单位 ms */
#define ADC_TIMEOUT_MS       10U

/* ---- 采样调度: "电压组 / 电流组"的时间占比 (任意正整数, 表示相对比例) ----
 * 恒压模式把更多采样时间给电压, 恒流模式把更多给电流.
 * 例如 4:1 = 每 4+1 个循环拍里, 4 拍采电压组 (Vin/Vout), 1 拍采电流组 (Iin/Iout);
 * 想改占比直接改这四个数 (例如 9:1), 两边都配 0 时退化为只采电压组 */
#define CV_VOLT_SLOTS        4U    /* 恒压模式: 电压组时隙数 */
#define CV_CURR_SLOTS        1U    /* 恒压模式: 电流组时隙数 */
#define CC_VOLT_SLOTS        1U    /* 恒流模式: 电压组时隙数 */
#define CC_CURR_SLOTS        4U    /* 恒流模式: 电流组时隙数 */

/* ---- V2.06 实测校准: 系数与函数统一在 measurement_calibration.h ----
 * 电流: 20mΩ × INA199A1 50 倍的名义电流, 再做二次校正;
 * 输出电压: 分压名义值 + 电流相关修正, 估算带载端子电压.
 * 输入参考表接在 D4 前、PA7 接 D4 后, 不把二极管压降当 ADC 误差.
 * 原始数据、拟合与范围见 calibration/README.md; 不做上电自动归零. */
#define VIN_ADC_SCALE        CAL_VIN_ADC_SCALE
#define VOUT_ADC_SCALE       CAL_VOUT_ADC_SCALE

/* ---- DAC -> 输出电压传递关系 (Vout ≈ A - B × V_DAC, 实测后修正) ----
 * R10=10k / R13=2.2k / R11=1.2k, LM2596 Vfb 典型 1.23V:
 * A=1.23*(1+10/1.2+10/2.2), B=10/2.2. 实测后再修正.
 * 这是反馈设定范围, 不保证 16V 输入时功率级能带载输出 15V. */
#define VOUT_DAC_A           17.07091f
#define VOUT_DAC_B           4.545455f
#define DAC_VMAX             3.3f    /* DAC 参考电压, 用于码值换算 */
#define DAC_OUTPUT_MIN_V     0.2f    /* 缓冲开启后的规定输出范围 */
#define DAC_OUTPUT_MAX_V     (DAC_VMAX - 0.2f)
#define DAC_FULL_SCALE       4095.0f /* G431 12 位 DAC 满量程码值 */

/* ---- 运行目标与步进 (题目 1.2 / 2.2 / 恒流模式) ---- */
#define VOUT_TARGET_DEFAULT  8.0f    /* 恒压模式默认目标电压 */
#define VOUT_TARGET_MIN      5.0f    /* 步进调压下限 */
#define VOUT_TARGET_MAX      15.0f   /* 步进调压上限 */
#define VOUT_STEP            1.0f    /* 步进 1V */
#define IOUT_TARGET_DEFAULT  1.0f    /* 恒流模式默认目标电流 */
#define IOUT_TARGET_MIN      0.2f
#define IOUT_TARGET_MAX      2.0f
#define IOUT_STEP            0.1f

/* ---- 保护阈值 (题目 2.1) ---- */
#define OCP_TRIP_A           2.3f    /* Iout 超过 -> 过流保护 (手动解除) */
#define OCP_CONFIRM_MS       50U     /* 过流需"持续"该时长才锁存保护, 防负载瞬变误触发 */
#define UVP_TRIP_V           12.5f   /* Vin 持续低于 -> 欠压保护 (自动恢复) */
#define UVP_CONFIRM_MS       3000U   /* 运行中欠压需连续确认 3 秒, 正常样本清零计时 */
#define UVP_RECOVER_V        13.0f   /* Vin 回升到该值 -> 自动恢复 (留迟滞) */
#define PROT_BLANK_MS        300U    /* 合闸后的过流屏蔽期; 欠压/ADC 故障不屏蔽 */

/* ---- 时序 ---- */
#define APP_LOOP_PERIOD_MS   10U     /* 主循环节拍 */
#define DISPLAY_REFRESH_MS   100U    /* 屏幕刷新周期 */
#define DISPLAY_AVG_TICKS    3U      /* 显示平均窗口: 最近 3 次刷新 (约 300ms) */
#define KEY_DEBOUNCE_MS      20U     /* 按键消抖时间 */
#define KEY_LONG_MS          800U    /* 长按判定时间 */
#define BEEP_ON_MS           200U    /* 开机蜂鸣: 每声时长 */
#define BEEP_GAP_MS          500U    /* 开机蜂鸣: 两声间隔 (题目 1.1) */
#define BEEP_STARTUP_TIMES   3U      /* 开机蜂鸣次数 */

/* ---- 按键: 新图 K1=PB15 升, K2=PB14 降, K3=PB13 功能; 按下=低 ---- */
#define KEY_PORT             GPIOB
#define KEY_UP_PIN           GPIO_PIN_15
#define KEY_DOWN_PIN         GPIO_PIN_14
#define KEY_FUNC_PIN         GPIO_PIN_13
#define KEY_IDX_UP           0U
#define KEY_IDX_DOWN         1U
#define KEY_IDX_FUNC         2U

/* ---- OLED 布局 (6×8 字体; 行距 8px) ---- */
#define OLED_FONT            OLED_6X8
/* 数值列 x: 标签区最宽 4 字符 (24px) + 12px 间隙 */
#define OLED_VALUE_X         36U
#define OLED_FLOAT_INT_LEN   2U
#define OLED_FLOAT_FRA_LEN   2U
/* 单位列 x: 数值最宽 "+15.00" 6 字符 = 36px, 后留 6px */
#define OLED_UNIT_X          78U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
ADC_HandleTypeDef hadc2;

DAC_HandleTypeDef hdac1;

/* USER CODE BEGIN PV */
/* 引脚 -> 通道 (以 2026-10-20 G431 原理图为准, 旧使用手册不适用):
 *   PA7 = INA_V_IN  (输入电压 ÷7),  PA6 = INA_V_OUT (输出电压 ÷7)   -> ADC2
 *   PB1 = 输入电流,                  PB0 = 输出电流                   -> ADC1
 * 输出通断 MOS: PA1 (高=导通);  蜂鸣器 MOS: PA2 (高=响)
 * 按键: PB15=K1 升, PB14=K2 降, PB13=K3 功能键 (上拉输入, 按下=低) */
/* DAC: PA4 = DAC1_OUT1, VREF=3.3V */

/* ---- 状态机 ---- */
static AppState_t appState = APP_ST_SELF_TEST;
static AppState_t stateBeforeProtect = APP_ST_CV;  /* 保护前的运行模式, 恢复用 */
static uint32_t   protBlankUntil = 0U;             /* 合闸屏蔽期截止时刻 */
static uint8_t    vinFresh = 0U;                   /* 有新的电压采样 (供欠压确认窗口消费) */
static uint8_t    uvpTracking = 0U;                /* 正在累计连续欠压时间 */
static uint32_t   uvpSince = 0U;                   /* 本轮连续欠压的起始时刻 */
static uint8_t    ioutFresh = 0U;                  /* 有新的电流采样 (供过流确认窗口消费) */
static uint8_t    ocpTracking = 0U;                /* 正在累计过流持续时间 */
static uint32_t   ocpSince = 0U;                   /* 本轮连续过流的起始时刻 */

/* ---- 设定值 ---- */
static float targetV = VOUT_TARGET_DEFAULT;
static float targetI = IOUT_TARGET_DEFAULT;

/* ---- 测量值 (已按标定系数换算) ---- */
static float measVin  = 0.0f;
static float measVout = 0.0f;
static float measVoutNominal = 0.0f; /* PA6 名义电压, 避免电流刷新时重复校正 */
static float measIin  = 0.0f;
static float measIout = 0.0f;

/* ---- 按键 (K1 / K2 / K3) ---- */
static Key_t keys[3] =
{
  { .port = KEY_PORT, .pin = KEY_UP_PIN,   .stable = 1U, .rawLast = 1U },
  { .port = KEY_PORT, .pin = KEY_DOWN_PIN, .stable = 1U, .rawLast = 1U },
  { .port = KEY_PORT, .pin = KEY_FUNC_PIN, .stable = 1U, .rawLast = 1U },
};

/* ---- 循环节拍 ---- */
static uint32_t lastDisplayTick = 0U;
/* ---- 显示平滑窗口 (行: Vin / Vout / Iin / Iout) ---- */
static float   dispHist[4][DISPLAY_AVG_TICKS];
static uint8_t dispHistIdx = 0U;
static uint8_t dispHistFilled = 0U;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC2_Init(void);
static void MX_DAC1_Init(void);
static void MX_ADC1_Init(void);
/* USER CODE BEGIN PFP */
static uint8_t ADC_ReadVoltage(ADC_HandleTypeDef *hadc, uint32_t channel, float *voltage);

/* 应用层函数 */
static void  App_SetState(AppState_t next);
static void  App_OutputEnable(uint8_t on);
static void  App_Buzzer(uint8_t on);
static void  App_PlayStartupBeeps(void);
static uint8_t App_SampleVoltageGroup(void);
static uint8_t App_SampleCurrentGroup(void);
static void  App_AdcFault(void);
static void  App_SampleScheduler(void);
static void  App_ScanKeys(void);
static void  App_HandleKeyEvent(uint8_t idx, KeyEvent_t ev);
static void  App_ControlStep(void);
static void  App_ProtectionCheck(void);
static void  App_UpdateDisplay(void);
static void  App_DisplayAverage(float *vin, float *vout, float *iin, float *iout);
static float App_TargetToDac(float vout);
static void  App_DacSetVoltage(float voltage);
static float App_Clamp(float v, float lo, float hi);
static char *App_StateText(AppState_t st);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_ADC2_Init();
  MX_DAC1_Init();
  MX_ADC1_Init();
  /* USER CODE BEGIN 2 */
  /* ---- OLED: 软件 I2C, SCL=PB3, SDA=PB4, 内部含清屏 ---- */
  OLED_Init();

  /* 输出 MOS 默认关断 (上电即待机无输出, 题目 1.1), 显式再关一次双保险 */
  App_OutputEnable(0U);
  App_Buzzer(0U);
  App_UpdateDisplay();                 /* 先显示 "Power Manager" + SELF TEST */

  /* ---- DAC1: PA4, 12 位, 参考 3.3V ---- */
  if (HAL_DAC_Start(&hdac1, DAC_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* 按理论传递关系开环预置到 8V 目标 (DAC ≈ 1.996V), 之后合闸过冲最小;
     实测后可修正 VOUT_DAC_A/B, 或等闭环接管 */
  App_DacSetVoltage(App_TargetToDac(VOUT_TARGET_DEFAULT));

  /* ADC 校准: 上电后各执行一次, 可消除内部电容误差, 提高转换精度 */
  if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_ADCEx_Calibration_Start(&hadc2, ADC_SINGLE_ENDED) != HAL_OK)
  {
    Error_Handler();
  }
  /* 采样失败保留 ADC 故障状态, 不把故障覆盖成待机. */
  if ((App_SampleVoltageGroup() != 0U) && (App_SampleCurrentGroup() != 0U))
  {
    App_UpdateDisplay();
    App_PlayStartupBeeps();
    /* 蜂鸣后重新采集, 避免继续显示启动过程的旧读数. */
    if ((App_SampleVoltageGroup() != 0U) && (App_SampleCurrentGroup() != 0U))
    {
      App_SetState(APP_ST_STANDBY);
    }
  }
  App_UpdateDisplay();
/* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    uint32_t now = HAL_GetTick();

    /* 1. 按键扫描 + 事件分发 (K1/K2/K3 分工见 App_HandleKeyEvent 注释) */
    App_ScanKeys();
    for (uint8_t i = 0U; i < 3U; i++)
    {
      if (keys[i].event != KEY_EV_NONE)
      {
        App_HandleKeyEvent(i, keys[i].event);
      }
    }

    /* 2. 采样调度: 按当前模式的"电压/电流时间占比"轮流采两组信号 */
    App_SampleScheduler();

    /* 3. 闭环控制 (占位): 后续 PID 在这里实现 */
    App_ControlStep();

    /* 4. 保护检查: 过流 / 欠压 + 欠压自动恢复 (题目 2.1) */
    App_ProtectionCheck();

    /* 5. 显示刷新 (固定周期, 不用每拍都刷) */
    if ((now - lastDisplayTick) >= DISPLAY_REFRESH_MS)
    {
      lastDisplayTick = now;
      App_UpdateDisplay();
    }

    /* 6. 主循环节拍 */
    HAL_Delay(APP_LOOP_PERIOD_MS);
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV2;
  RCC_OscInitStruct.PLL.PLLN = 85;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_MultiModeTypeDef multimode = {0};
  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV8;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.GainCompensation = 0;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc1.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the ADC multi-mode
  */
  multimode.Mode = ADC_MODE_INDEPENDENT;
  if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &multimode) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_15;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_640CYCLES_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief ADC2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC2_Init(void)
{

  /* USER CODE BEGIN ADC2_Init 0 */

  /* USER CODE END ADC2_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC2_Init 1 */

  /* USER CODE END ADC2_Init 1 */

  /** Common config
  */
  hadc2.Instance = ADC2;
  hadc2.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV8;
  hadc2.Init.Resolution = ADC_RESOLUTION_12B;
  hadc2.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc2.Init.GainCompensation = 0;
  hadc2.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc2.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc2.Init.LowPowerAutoWait = DISABLE;
  hadc2.Init.ContinuousConvMode = DISABLE;
  hadc2.Init.NbrOfConversion = 1;
  hadc2.Init.DiscontinuousConvMode = DISABLE;
  hadc2.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc2.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc2.Init.DMAContinuousRequests = DISABLE;
  hadc2.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc2.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc2) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_3;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_640CYCLES_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc2, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC2_Init 2 */

  /* USER CODE END ADC2_Init 2 */

}

/**
  * @brief DAC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_DAC1_Init(void)
{

  /* USER CODE BEGIN DAC1_Init 0 */

  /* USER CODE END DAC1_Init 0 */

  DAC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN DAC1_Init 1 */

  /* USER CODE END DAC1_Init 1 */

  /** DAC Initialization
  */
  hdac1.Instance = DAC1;
  if (HAL_DAC_Init(&hdac1) != HAL_OK)
  {
    Error_Handler();
  }

  /** DAC channel OUT1 config
  */
  sConfig.DAC_HighFrequency = DAC_HIGH_FREQUENCY_INTERFACE_MODE_AUTOMATIC;
  sConfig.DAC_DMADoubleDataMode = DISABLE;
  sConfig.DAC_SignedFormat = DISABLE;
  sConfig.DAC_SampleAndHold = DAC_SAMPLEANDHOLD_DISABLE;
  sConfig.DAC_Trigger = DAC_TRIGGER_NONE;
  sConfig.DAC_Trigger2 = DAC_TRIGGER_NONE;
  sConfig.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
  sConfig.DAC_ConnectOnChipPeripheral = DAC_CHIPCONNECT_EXTERNAL;
  sConfig.DAC_UserTrimming = DAC_TRIMMING_FACTORY;
  if (HAL_DAC_ConfigChannel(&hdac1, &sConfig, DAC_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN DAC1_Init 2 */

  /* USER CODE END DAC1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1|GPIO_PIN_2, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3|GPIO_PIN_4, GPIO_PIN_SET);

  /*Configure GPIO pins : PA1 PA2 */
  GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_2;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : PB11 PB9 */
  GPIO_InitStruct.Pin = GPIO_PIN_11|GPIO_PIN_9;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : PA8 */
  GPIO_InitStruct.Pin = GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : PB3 PB4 */
  GPIO_InitStruct.Pin = GPIO_PIN_3|GPIO_PIN_4;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* 新图按键迁至 PB13/PB14/PB15; 与 .ioc 同步, 写在用户区以便重新生成保留.
     旧生成区的 PA8/PB9/PB11 已不接按键, 将它们恢复为无上下拉模拟输入. */
  GPIO_InitStruct.Pin = GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
  GPIO_InitStruct.Pin = GPIO_PIN_9 | GPIO_PIN_11;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
  GPIO_InitStruct.Pin = KEY_UP_PIN | KEY_DOWN_PIN | KEY_FUNC_PIN;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(KEY_PORT, &GPIO_InitStruct);
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
/**
  * @brief  读取指定 ADC 的一个通道, 返回该通道的电压值 (已做均值滤波).
  * @note   CubeMX 中 ADC 配置为 "ScanConvMode = Disable, NbrOfConversion = 1",
  *         因此这里在每次转换前把目标通道写入规则组 Rank1, 逐个通道轮流采集,
  *         不需要修改 MX_ADCx_Init() 生成的代码, 也不需要 DMA.
  *         G431 的 ADC 通道是单端/差分可选的, 这里固定用单端.
  * @param  hadc    : ADC 句柄, hadc1 或 hadc2
  * @param  channel : ADC 通道宏, 例如 ADC_CHANNEL_15
  * @param  voltage : 成功时写入 ADC 引脚电压 (V); 失败时不改旧值
  * @retval 1=成功, 0=HAL 错误; 失败不能伪装成零电流
  */
static uint8_t ADC_ReadVoltage(ADC_HandleTypeDef *hadc, uint32_t channel, float *voltage)
{
  ADC_ChannelConfTypeDef sConfig = {0};
  uint32_t sum = 0U;
  uint32_t i;

  /* 停止并关闭 ADC, 保证修改通道配置时 ADC 处于空闲状态 */
  if (HAL_ADC_Stop(hadc) != HAL_OK)
  {
    return 0U;
  }

  /* 把目标通道配置到规则组的第 1 个转换 */
  sConfig.Channel = channel;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLING_TIME;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0U;
  if (HAL_ADC_ConfigChannel(hadc, &sConfig) != HAL_OK)
  {
    return 0U;
  }

  /* 通道刚切换后的第一次转换会受采样保持电容残留电荷影响, 丢弃;
     之后再做 ADC_OVERSAMPLING 次求平均. 高阻分压下这一条对消除串扰很关键 */
  for (i = 0U; i < (ADC_OVERSAMPLING + 1U); i++)
  {
    if (HAL_ADC_Start(hadc) != HAL_OK)
    {
      HAL_ADC_Stop(hadc);
      return 0U;
    }
    if (HAL_ADC_PollForConversion(hadc, ADC_TIMEOUT_MS) != HAL_OK)
    {
      HAL_ADC_Stop(hadc);
      return 0U;
    }
    if (i > 0U)                    /* 丢弃通道切换后的第一个样本 */
    {
      sum += HAL_ADC_GetValue(hadc);
    }
  }

  if (HAL_ADC_Stop(hadc) != HAL_OK)
  {
    return 0U;
  }

  /* 12 位 ADC: V = 码值 / 4095 * VREF */
  *voltage = ((float)sum / (float)ADC_OVERSAMPLING) * ADC_VREF / ADC_FULL_SCALE;
  return 1U;
}

/* --------------------------------------------------------------------------
 * 应用层实现
 * -------------------------------------------------------------------------- */

static float App_Clamp(float v, float lo, float hi)
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

/* 目标电压 -> DAC 输出电压. 理论关系 Vout ≈ VOUT_DAC_A - VOUT_DAC_B × V_DAC,
 * 反解并限幅到缓冲模式输出范围 [0.2, 3.1]V; 实测后修正 A/B */
static float App_TargetToDac(float vout)
{
  return App_Clamp((VOUT_DAC_A - vout) / VOUT_DAC_B,
                   DAC_OUTPUT_MIN_V, DAC_OUTPUT_MAX_V);
}

/* DAC 电压 -> 12 位码值并写入 DAC1_OUT1 (PA4).
 * 3.3V 对应 4095, 四舍五入; 同时挡住 NaN / 越界, 避免浮点转整数未定义行为 */
static void App_DacSetVoltage(float voltage)
{
  float code;

  /* 负增益通路: 非有限值取高 DAC 电压以降低输出, 不要错误归零推高输出. */
  if (!isfinite(voltage))
  {
    voltage = DAC_OUTPUT_MAX_V;
  }
  voltage = App_Clamp(voltage, DAC_OUTPUT_MIN_V, DAC_OUTPUT_MAX_V);

  code = voltage * DAC_FULL_SCALE / DAC_VMAX + 0.5f;
  if (code >= DAC_FULL_SCALE)
  {
    code = DAC_FULL_SCALE;
  }
  if (HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, (uint32_t)code) != HAL_OK)
  {
    Error_Handler();
  }
}

/* 输出通断 MOS: PA1 高电平导通 */
static void App_OutputEnable(uint8_t on)
{
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, (on != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* 蜂鸣器 MOS: PA2 高电平导通 */
static void App_Buzzer(uint8_t on)
{
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, (on != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* 开机蜂鸣: 响 BEEP_ON_MS, 停 BEEP_GAP_MS, 共 BEEP_STARTUP_TIMES 次 (题目 1.1).
 * 只在启动阶段调用, 阻塞可接受 */
static void App_PlayStartupBeeps(void)
{
  uint8_t i;

  for (i = 0U; i < BEEP_STARTUP_TIMES; i++)
  {
    App_Buzzer(1U);
    HAL_Delay(BEEP_ON_MS);
    App_Buzzer(0U);
    HAL_Delay(BEEP_GAP_MS);
  }
}

/* 采电压组: Vin 保留 D4 后采样; Vout 使用最新电流估算端子电压. */
static uint8_t App_SampleVoltageGroup(void)
{
  float vin;
  float vout;
  if ((ADC_ReadVoltage(&hadc2, ADC_CHANNEL_4, &vin) == 0U) ||
      (ADC_ReadVoltage(&hadc2, ADC_CHANNEL_3, &vout) == 0U))
  {
    App_AdcFault();
    return 0U;
  }
  measVin = vin * VIN_ADC_SCALE;     /* PA7 */
  measVoutNominal = vout * VOUT_ADC_SCALE; /* PA6; 相对系统地 */
  measVout = Cal_Vout(measVoutNominal, measIout);
  vinFresh = 1U;
  return 1U;
}

/* 采电流组: 输入/输出电流 */
static uint8_t App_SampleCurrentGroup(void)
{
  float iin;
  float iout;
  if ((ADC_ReadVoltage(&hadc1, ADC_CHANNEL_12, &iin) == 0U) ||
      (ADC_ReadVoltage(&hadc1, ADC_CHANNEL_15, &iout) == 0U))
  {
    App_AdcFault();
    return 0U;
  }
  measIin = Cal_Iin(iin);
  measIout = Cal_Iout(iout);
  measVout = Cal_Vout(measVoutNominal, measIout);
  ioutFresh = 1U;               /* 电流数据已刷新, 供过流确认窗口消费 */
  return 1U;
}

static void App_AdcFault(void)
{
  ioutFresh = 0U;
  if (appState != APP_ST_PROT_ADC)
  {
    App_SetState(APP_ST_PROT_ADC);
  }
}

/* 采样调度器: 每个循环拍消费一个"时隙".
 * 恒压模式偏向电压 (CV_VOLT_SLOTS : CV_CURR_SLOTS), 恒流模式偏向电流 (CC_*),
 * 占比宏在文件头部, 想调就改那里 */
static void App_SampleScheduler(void)
{
  static uint8_t slot = 0U;
  uint8_t voltSlots;
  uint8_t currSlots;
  uint8_t total;

  voltSlots = (appState == APP_ST_CC) ? CC_VOLT_SLOTS : CV_VOLT_SLOTS;
  currSlots = (appState == APP_ST_CC) ? CC_CURR_SLOTS : CV_CURR_SLOTS;
  total = (uint8_t)(voltSlots + currSlots);

  if (total == 0U)                 /* 防呆: 宏都配 0 时退化为全采电压 */
  {
    App_SampleVoltageGroup();
    return;
  }
  if (slot >= total)
  {
    slot = 0U;
  }

  if (slot < voltSlots)
  {
    App_SampleVoltageGroup();
  }
  else
  {
    App_SampleCurrentGroup();
  }
  slot++;
}

/* 按键扫描: 上拉输入, 按下=低. 消抖 KEY_DEBOUNCE_MS, 长按 KEY_LONG_MS.
 * 短按在松手时产生; 长按在按住达到阈值时产生一次, 之后松手不再产生短按 */
static void App_ScanKeys(void)
{
  uint32_t now = HAL_GetTick();
  uint8_t i;

  for (i = 0U; i < 3U; i++)
  {
    uint8_t raw = (HAL_GPIO_ReadPin(keys[i].port, keys[i].pin) == GPIO_PIN_RESET) ? 0U : 1U;

    keys[i].event = KEY_EV_NONE;

    if (raw != keys[i].rawLast)
    {
      keys[i].rawLast = raw;
      keys[i].changedAt = now;
    }
    /* 用真实毫秒数消抖/判长按, OLED 刷新和 ADC 耗时不计成固定 10ms. */
    if ((raw != keys[i].stable) &&
        ((uint32_t)(now - keys[i].changedAt) >= KEY_DEBOUNCE_MS))
    {
      keys[i].stable = raw;
      if (raw == 0U)
      {
        keys[i].pressedAt = now;
        keys[i].longFired = 0U;
      }
      else if (keys[i].longFired == 0U)
      {
        keys[i].event = KEY_EV_SHORT;
      }
    }
    if ((raw == 0U) && (keys[i].stable == 0U) && (keys[i].longFired == 0U) &&
        ((uint32_t)(now - keys[i].pressedAt) >= KEY_LONG_MS))
    {
      keys[i].longFired = 1U;
      keys[i].event = KEY_EV_LONG;
    }
  }
}

/* 按键事件分发 —— 按键分工:
 *   待机:      K3 短按 -> 进入恒压 (待机 <-> 恒压 的双向切换键, 题目 1.2)
 *   恒压运行:  K1/K2 短按 -> 目标 ±1V;  K3 短按 -> 回待机;  K3 长按 -> 切恒流
 *   恒流运行:  K1/K2 短按 -> 目标 ±0.1A; K3 短按 -> 回待机; K3 长按 -> 切恒压
 *   过流保护:  K3 短按 -> 解除, 回保护前模式 (输入仍欠压则转欠压保护, 题目 2.1)
 *   欠压保护:  K3 短按 -> 暂时静音; 输入恢复正常后自动回保护前模式
 * TODO: K1/K2 长按连发 (快速调节), 后续再加 */
static void App_HandleKeyEvent(uint8_t idx, KeyEvent_t ev)
{
  if (ev == KEY_EV_NONE)
  {
    return;
  }

  switch (appState)
  {
    case APP_ST_STANDBY:
      if ((idx == KEY_IDX_FUNC) && (ev == KEY_EV_SHORT))
      {
        App_SetState(APP_ST_CV);
      }
      break;

    case APP_ST_CV:
      if ((idx == KEY_IDX_UP) && (ev == KEY_EV_SHORT))
      {
        targetV = App_Clamp(targetV + VOUT_STEP, VOUT_TARGET_MIN, VOUT_TARGET_MAX);
        App_DacSetVoltage(App_TargetToDac(targetV)); /* 同步更新 DAC, 步进才真正生效 */
      }
      else if ((idx == KEY_IDX_DOWN) && (ev == KEY_EV_SHORT))
      {
        targetV = App_Clamp(targetV - VOUT_STEP, VOUT_TARGET_MIN, VOUT_TARGET_MAX);
        App_DacSetVoltage(App_TargetToDac(targetV)); /* 同步更新 DAC, 步进才真正生效 */
      }
      else if (idx == KEY_IDX_FUNC)
      {
        if (ev == KEY_EV_SHORT)
        {
          App_SetState(APP_ST_STANDBY);
        }
        else
        {
          App_SetState(APP_ST_CC);              /* 长按切恒流 */
        }
      }
      else
      {
        /* 其余组合暂不响应 */
      }
      break;

    case APP_ST_CC:
      if ((idx == KEY_IDX_UP) && (ev == KEY_EV_SHORT))
      {
        targetI = App_Clamp(targetI + IOUT_STEP, IOUT_TARGET_MIN, IOUT_TARGET_MAX);
      }
      else if ((idx == KEY_IDX_DOWN) && (ev == KEY_EV_SHORT))
      {
        targetI = App_Clamp(targetI - IOUT_STEP, IOUT_TARGET_MIN, IOUT_TARGET_MAX);
      }
      else if (idx == KEY_IDX_FUNC)
      {
        if (ev == KEY_EV_SHORT)
        {
          App_SetState(APP_ST_STANDBY);
        }
        else
        {
          App_SetState(APP_ST_CV);              /* 长按切恒压 */
        }
      }
      else
      {
        /* 其余组合暂不响应 */
      }
      break;

    case APP_ST_PROT_OCP:
      if ((idx == KEY_IDX_FUNC) && (ev == KEY_EV_SHORT))
      {
        if (measVin < UVP_TRIP_V)
        {
          App_SetState(APP_ST_PROT_UVP);        /* 输入还欠压, 先转欠压保护 */
        }
        else
        {
          App_SetState(stateBeforeProtect);     /* 手动解除, 恢复正常工作 */
        }
      }
      break;

    case APP_ST_PROT_UVP:
      if ((idx == KEY_IDX_FUNC) && (ev == KEY_EV_SHORT))
      {
        App_Buzzer(0U);                         /* 暂时消音, 恢复逻辑照常 */
      }
      break;

    case APP_ST_PROT_ADC:
      if ((idx == KEY_IDX_FUNC) && (ev == KEY_EV_SHORT))
      {
        /* 故障恢复后先回待机, 再次按键才合闸; 不能自动恢复功率输出. */
        if ((App_SampleVoltageGroup() != 0U) && (App_SampleCurrentGroup() != 0U))
        {
          App_SetState(APP_ST_STANDBY);
        }
      }
      break;

    default:
      break;
  }
}

/* 状态切换入口: 统一处理"输出通断 / DAC 预置 / 报警 / 屏蔽期"等进入动作 */
static void App_SetState(AppState_t next)
{
  /* 合闸前取得新测量, 不允许拿故障值或过时 Vin 开输出. */
  if ((next == APP_ST_CV) || (next == APP_ST_CC))
  {
    if ((App_SampleVoltageGroup() == 0U) || (App_SampleCurrentGroup() == 0U))
    {
      return;  /* 采样函数已切到 ADC 故障并关断输出 */
    }
    if (measVin < UVP_TRIP_V)
    {
      stateBeforeProtect = next;
      next = APP_ST_PROT_UVP;
    }
  }

  /* 离开运行模式时记下它, 供保护恢复用 */
  if ((appState == APP_ST_CV) || (appState == APP_ST_CC))
  {
    stateBeforeProtect = appState;
  }

  appState = next;

  /* 状态切换后重新确认欠压, 不沿用上次运行的计时或合闸前样本. */
  vinFresh = 0U;
  uvpTracking = 0U;

  /* 状态切换后显示平均窗口重新起算, 避免上一状态的读数混进新状态 */
  dispHistFilled = 0U;
  dispHistIdx = 0U;

  /* 进入运行模式: 过流确认窗口重新起算, 清掉上一段的残留 */
  if ((next == APP_ST_CV) || (next == APP_ST_CC))
  {
    ioutFresh = 0U;
    ocpTracking = 0U;
  }

  switch (next)
  {
    case APP_ST_SELF_TEST:
    case APP_ST_STANDBY:
      App_OutputEnable(0U);                     /* 待机无输出 (题目 1.1) */
      App_Buzzer(0U);
      break;

    case APP_ST_CV:
      targetV = App_Clamp(targetV, VOUT_TARGET_MIN, VOUT_TARGET_MAX);
      App_DacSetVoltage(App_TargetToDac(targetV)); /* 开环预置, 减小合闸过冲 */
      App_OutputEnable(1U);
      App_Buzzer(0U);
      protBlankUntil = HAL_GetTick() + PROT_BLANK_MS;
      break;

    case APP_ST_CC:
      /* 恒流模式先保持当前 DAC 值, 让闭环往目标电流靠 (闭环待实现) */
      App_OutputEnable(1U);
      App_Buzzer(0U);
      protBlankUntil = HAL_GetTick() + PROT_BLANK_MS;
      break;

    case APP_ST_PROT_OCP:
      App_OutputEnable(0U);                     /* 关输出 */
      App_Buzzer(1U);                           /* 报警 */
      break;

    case APP_ST_PROT_UVP:
    case APP_ST_PROT_ADC:
      App_OutputEnable(0U);
      App_Buzzer(1U);
      break;

    default:
      break;
  }
}

/* 闭环控制: 框架占位, PID 下一版实现.
 * 恒压: 误差 = targetV - measVout;  恒流: 误差 = targetI - measIout;
 * 都作用在同一条 DAC 通路上, 注意增益方向为负 (DAC 电压升高 -> 输出降低).
 * 建议: 每拍限幅小步长, 配合 App_SetState 的开环预置, 避免合闸/恢复过冲 */
static void App_ControlStep(void)
{
  /* TODO: PID 闭环 */
}

/* 保护检查 (题目 2.1): 过流 -> 锁存到手动解除; 欠压 -> 自动恢复.
 * 运行中欠压按新电压样本连续确认 UVP_CONFIRM_MS, 正常样本清零计时;
 * 合闸后 PROT_BLANK_MS 内仅屏蔽过流, 欠压确认照常计时;
 * 过流只在新样本上判定, 需持续 OCP_CONFIRM_MS 才锁存, 负载瞬变/尖峰不会误触发 */
static void App_ProtectionCheck(void)
{
  if ((appState == APP_ST_CV) || (appState == APP_ST_CC))
  {
    if (vinFresh != 0U)
    {
      vinFresh = 0U;
      if (measVin < UVP_TRIP_V)
      {
        if (uvpTracking == 0U)
        {
          uvpTracking = 1U;
          uvpSince = HAL_GetTick();
        }
        else if ((uint32_t)(HAL_GetTick() - uvpSince) >= UVP_CONFIRM_MS)
        {
          App_SetState(APP_ST_PROT_UVP);
          return;
        }
      }
      else
      {
        uvpTracking = 0U;
      }
    }
    if ((int32_t)(HAL_GetTick() - protBlankUntil) < 0)
    {
      return;                                 /* 屏蔽期内 */
    }

    /* 过流确认: 任一"正常"的新样本立即清零累计, 只认持续过流 */
    if (ioutFresh != 0U)
    {
      ioutFresh = 0U;
      if (measIout > OCP_TRIP_A)
      {
        if (ocpTracking == 0U)
        {
          ocpTracking = 1U;
          ocpSince = HAL_GetTick();
        }
        else if ((uint32_t)(HAL_GetTick() - ocpSince) >= OCP_CONFIRM_MS)
        {
          App_SetState(APP_ST_PROT_OCP);
          return;
        }
      }
      else
      {
        ocpTracking = 0U;
      }
    }

  }
  else if (appState == APP_ST_PROT_UVP)
  {
    if (measVin >= UVP_RECOVER_V)
    {
      App_SetState(stateBeforeProtect);       /* 输入恢复 -> 自动回到保护前模式 */
    }
  }
  else
  {
    /* 待机 / 过流保护: 不需要自动动作 */
  }
}

static char *App_StateText(AppState_t st)
{
  switch (st)
  {
    case APP_ST_SELF_TEST: return "SELF TEST";
    case APP_ST_STANDBY:   return "STANDBY";
    case APP_ST_CV:        return "CV MODE";
    case APP_ST_CC:        return "CC MODE";
    case APP_ST_PROT_OCP:  return "OCP ALARM!";
    case APP_ST_PROT_UVP:  return "UVP ALARM!";
    case APP_ST_PROT_ADC:  return "ADC FAULT!";
    default:               return "?";
  }
}

/* 显示平滑: 把当前测量值压入窗口, 返回最近 DISPLAY_AVG_TICKS 次的平均.
 * 仅用于屏幕显示; 保护判断仍用实时 meas* 值, 不增加保护延迟.
 * 首次调用/状态切换后用当前值填满窗口, 避免从 0 或旧值慢慢爬升. */
static void App_DisplayAverage(float *vin, float *vout, float *iin, float *iout)
{
  float vals[4];
  float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  uint8_t ch;
  uint8_t k;

  vals[0] = measVin;
  vals[1] = measVout;
  vals[2] = measIin;
  vals[3] = measIout;

  if (dispHistFilled == 0U)
  {
    for (ch = 0U; ch < 4U; ch++)
    {
      for (k = 0U; k < DISPLAY_AVG_TICKS; k++)
      {
        dispHist[ch][k] = vals[ch];
      }
    }
    dispHistFilled = DISPLAY_AVG_TICKS;
    dispHistIdx = 0U;
  }
  else
  {
    for (ch = 0U; ch < 4U; ch++)
    {
      dispHist[ch][dispHistIdx] = vals[ch];
    }
    dispHistIdx = (uint8_t)((dispHistIdx + 1U) % DISPLAY_AVG_TICKS);
    if (dispHistFilled < DISPLAY_AVG_TICKS)
    {
      dispHistFilled++;
    }
  }

  for (ch = 0U; ch < 4U; ch++)
  {
    for (k = 0U; k < dispHistFilled; k++)
    {
      sum[ch] += dispHist[ch][k];
    }
    sum[ch] /= (float)dispHistFilled;
  }

  *vin  = sum[0];
  *vout = sum[1];
  *iin  = sum[2];
  *iout = sum[3];
}

/* 屏幕布局 (6×8 字体, 行距 8px):
 *   y=0  "Power Manager" 固定标题 (题目 1.1)
 *   y=8  工作状态
 *   y=16 Vin / y=24 Vout / y=32 Iin / y=40 Iout   四个测量值 (题目 1.7)
 *   y=48 设定值 (恒压=目标电压, 恒流=目标电流)
 *   y=56 按键操作提示 */
static void App_UpdateDisplay(void)
{
  char *hint;
  float voutDisp;
  float dispVin;
  float dispVout;
  float dispIin;
  float dispIout;

  /* 显示平滑: 取最近 DISPLAY_AVG_TICKS 次刷新的平均, 抑制末位跳动 */
  App_DisplayAverage(&dispVin, &dispVout, &dispIin, &dispIout);

  OLED_Clear();
  OLED_ShowString(0, 0,  "Power Manager", OLED_FONT);
  OLED_ShowString(0, 8,  App_StateText(appState), OLED_FONT);

  OLED_ShowString(0, 16, "Vin", OLED_FONT);
  OLED_ShowFloatNum(OLED_VALUE_X, 16, dispVin, OLED_FLOAT_INT_LEN, OLED_FLOAT_FRA_LEN, OLED_FONT);
  OLED_ShowChar(OLED_UNIT_X, 16, 'V', OLED_FONT);

  /* PA6 采的是输出正端对系统地; Q3 在负端, 关断时负端浮空.
     非运行状态的 0.00V 代表输出已禁用, 并非实测端子压差;
     正端仍可能带电. 运行读数已按实测电流补偿为端子电压估计值. */
  voutDisp = ((appState == APP_ST_CV) || (appState == APP_ST_CC)) ? dispVout : 0.0f;
  OLED_ShowString(0, 24, "Vout", OLED_FONT);
  OLED_ShowFloatNum(OLED_VALUE_X, 24, voutDisp, OLED_FLOAT_INT_LEN, OLED_FLOAT_FRA_LEN, OLED_FONT);
  OLED_ShowChar(OLED_UNIT_X, 24, 'V', OLED_FONT);

  OLED_ShowString(0, 32, "Iin", OLED_FONT);
  OLED_ShowFloatNum(OLED_VALUE_X, 32, dispIin, OLED_FLOAT_INT_LEN, OLED_FLOAT_FRA_LEN, OLED_FONT);
  OLED_ShowChar(OLED_UNIT_X, 32, 'A', OLED_FONT);

  OLED_ShowString(0, 40, "Iout", OLED_FONT);
  OLED_ShowFloatNum(OLED_VALUE_X, 40, dispIout, OLED_FLOAT_INT_LEN, OLED_FLOAT_FRA_LEN, OLED_FONT);
  OLED_ShowChar(OLED_UNIT_X, 40, 'A', OLED_FONT);

  OLED_ShowString(0, 48, "Set", OLED_FONT);
  if (appState == APP_ST_CC)
  {
    OLED_ShowFloatNum(OLED_VALUE_X, 48, targetI, OLED_FLOAT_INT_LEN, OLED_FLOAT_FRA_LEN, OLED_FONT);
    OLED_ShowChar(OLED_UNIT_X, 48, 'A', OLED_FONT);
  }
  else
  {
    OLED_ShowFloatNum(OLED_VALUE_X, 48, targetV, OLED_FLOAT_INT_LEN, OLED_FLOAT_FRA_LEN, OLED_FONT);
    OLED_ShowChar(OLED_UNIT_X, 48, 'V', OLED_FONT);
  }

  switch (appState)
  {
    case APP_ST_STANDBY:   hint = "K3=START"; break;
    case APP_ST_CV:        hint = "K1/K2=ADJ HOLD K3=CC"; break;
    case APP_ST_CC:        hint = "K1/K2=ADJ HOLD K3=CV"; break;
    case APP_ST_PROT_OCP:  hint = "K3=CLEAR"; break;
    case APP_ST_PROT_UVP:  hint = "K3=MUTE AUTO-RECOVER"; break;
    case APP_ST_PROT_ADC:  hint = "K3=RETRY TO STANDBY"; break;
    default:               hint = ""; break;
  }
  OLED_ShowString(0, 56, hint, OLED_FONT);

  OLED_Update();
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* HAL 初始化或 DAC 写入失败也应先断输出, 防止停在带电状态. */
  App_OutputEnable(0U);
  App_Buzzer(1U);
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
