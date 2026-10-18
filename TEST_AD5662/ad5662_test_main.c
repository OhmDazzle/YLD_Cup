/**
  ******************************************************************************
  * @file    ad5662_test_main.c
  * @brief   独立 AD5662 测试固件 (TEST_AD5662/): 单独编译、单独烧录.
  *
  *  本文件自成体系: 自带 main()、72MHz 时钟、GPIO 与 SPI2 初始化,
  *  不依赖 Core/Src/main.c, 只复用板级文件(启动/中断/MSP)和
  *  AD5662 / OLED 驱动. 构建目标: YLD_Cup_AD5662Test.
  *
  *  硬件:
  *    PB13 = SPI2_SCK -> AD5662 SCLK
  *    PB15 = SPI2_MOSI -> AD5662 DIN
  *    PB12 = GPIO      -> AD5662 SYNC (低有效)
  *    VREF = 3.3V, VFB 必须接 VOUT
  *
  *  安全:
  *    输出 MOS (PB10) 与蜂鸣器 (PA2) 始终保持关闭, 本固件只驱动 DAC.
  *
  *  界面:
  *    AD5662 TEST
  *    MODE 1/4 DC MANUAL
  *    TGT+0.00V STEP 0.1V
  *    CODE 00000 0x0000
  *    DAC+0.0000V
  *    SPI OK ERRCNT 000
  *    K1- K2+ K3:NEXT
  *    MOS OFF 9M CPHA1
  *
  *  按键:
  *    K1(PA8)  手动模式: 电压 -0.1V
  *    K2(B11)  手动模式: 电压 +0.1V
  *    K3(B9)   循环切换四种测试模式
  ******************************************************************************
  */

#include <stdint.h>
#include "stm32f1xx_hal.h"
#include "AD5662.h"
#include "OLED.h"

/* ---- 本固件的可调参数 ---- */
#define TEST_LOOP_MS           5U
#define TEST_DISPLAY_MS        200U
#define TEST_KEY_DEBOUNCE_MS   20U

#define TEST_DC_STEP_V         0.1f
#define TEST_DC_MAX_INDEX      33U     /* 0 .. 3.3V, 每档 0.1V */
#define TEST_STEP_HOLD_MS      750U
#define TEST_EXTREME_HOLD_MS   1500U
#define TEST_POWER_HOLD_MS     2000U

#define TEST_VREF              3.3f
#define TEST_POWER_CODE        0x8000U

/* AD5662 在 SCLK 下降沿锁存 DIN: CPOL=0, CPHA=1, 即 CubeMX 的 2 Edge.
   若需对比错误时序, 可临时改为 SPI_PHASE_1EDGE 后重新编译. */
#define TEST_SPI_PHASE         SPI_PHASE_2EDGE

/* ---- 测试模式 ---- */
typedef enum
{
  TEST_MODE_DC = 0,       /* 手动 0 ~ 3.3V */
  TEST_MODE_STEP,         /* 17 点阶梯: 0, 1/16 ... 15/16, 满量程 */
  TEST_MODE_EXTREMES,     /* 0 / 1/2 / 满量程循环 */
  TEST_MODE_POWER,        /* 正常 / 1k / 100k / 高阻循环 */
  TEST_MODE_COUNT
} TestMode_t;

/* ---- 按键 ---- */
typedef struct
{
  GPIO_TypeDef *port;
  uint16_t      pin;
  uint8_t       raw;        /* 最近一次原始电平: 1=按下 */
  uint8_t       stable;     /* 消抖后的电平: 1=按下 */
  uint32_t      changedAt;  /* 原始电平最后一次变化的时刻 */
  uint8_t       pressed;    /* 待处理的按下事件 */
} TestKey_t;

#define TEST_KEY_DOWN  0U
#define TEST_KEY_UP    1U
#define TEST_KEY_MODE  2U

static TestKey_t keys[3] =
{
  { GPIOA, GPIO_PIN_8,  0U, 0U, 0U, 0U },  /* K1: 电压 - */
  { GPIOB, GPIO_PIN_11, 0U, 0U, 0U, 0U },  /* K2: 电压 + */
  { GPIOB, GPIO_PIN_9,  0U, 0U, 0U, 0U },  /* K3: 切换模式 */
};

/* ---- 外设与测试状态 ---- */
static SPI_HandleTypeDef hspi2;
static AD5662_t dac;

static TestMode_t testMode = TEST_MODE_DC;
static uint8_t dcIndex = 0U;
static uint8_t stepIndex = 0U;
static uint8_t extremeIndex = 0U;
static uint8_t powerIndex = 0U;

static uint32_t phaseStarted = 0U;
static uint32_t lastDisplay = 0U;
static uint8_t displayDirty = 1U;

static HAL_StatusTypeDef lastStatus = HAL_OK;
static uint32_t spiFailCount = 0U;

/* 17 点: 0, 1/16 .. 15/16, 65535. 最后一个点单独用 65535 验证码值上边界 */
static const uint16_t stepCodes[] =
{
  0U, 4096U, 8192U, 12288U, 16384U, 20480U, 24576U, 28672U, 32768U,
  36864U, 40960U, 45056U, 49152U, 53248U, 57344U, 61440U, 65535U
};

static const uint16_t extremeCodes[] =
{
  0U, 32768U, 65535U
};

#define TEST_STEP_COUNT     (sizeof(stepCodes) / sizeof(stepCodes[0]))
#define TEST_EXTREME_COUNT  (sizeof(extremeCodes) / sizeof(extremeCodes[0]))

/* ---- 内部函数声明 ---- */
static void Error_Handler(void);
static void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_SPI2_Init(void);

static void Test_KeyScan(TestKey_t *key, uint32_t now);
static uint8_t Test_KeyTakePress(TestKey_t *key);
static void Test_SetMode(TestMode_t mode, uint32_t now);
static void Test_WriteCode(uint16_t code);
static void Test_WriteVoltage(float voltage);
static void Test_WritePowerDown(AD5662_PowerDown_t mode);
static void Test_UpdateAuto(uint32_t now);
static void Test_Draw(void);
static char *Test_ModeName(TestMode_t mode);
static char *Test_PowerName(uint8_t index);

/* ============================ 主函数 ============================ */

int main(void)
{
  uint32_t now;

  HAL_Init();
  SystemClock_Config();

  MX_GPIO_Init();
  MX_SPI2_Init();

  /* 先关掉输出级与蜂鸣器, 再初始化屏幕和 DAC.
     AD5662_Init 内部会写 0 码, 上电后 DAC 输出为 0V. */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2,  GPIO_PIN_RESET);

  OLED_Init();
  AD5662_Init(&dac, &hspi2, GPIOB, GPIO_PIN_12, TEST_VREF);

  /* AD5662_Init 不返回状态, 再写一次 0V 以获得本次 SPI 传输结果 */
  lastStatus = AD5662_SetVoltage(&dac, 0.0f);
  if (lastStatus != HAL_OK)
  {
    spiFailCount++;
  }

  Test_SetMode(TEST_MODE_DC, HAL_GetTick());
  lastDisplay = HAL_GetTick();

  while (1)
  {
    now = HAL_GetTick();

    /* 1. 按键: K1/K2 只在手动模式生效, K3 在任何模式都切换测试项 */
    Test_KeyScan(&keys[TEST_KEY_DOWN], now);
    Test_KeyScan(&keys[TEST_KEY_UP], now);
    Test_KeyScan(&keys[TEST_KEY_MODE], now);

    if (testMode == TEST_MODE_DC)
    {
      if (Test_KeyTakePress(&keys[TEST_KEY_DOWN]) != 0U)
      {
        if (dcIndex > 0U)
        {
          dcIndex--;
        }
        Test_WriteVoltage((float)dcIndex * TEST_DC_STEP_V);
      }
      if (Test_KeyTakePress(&keys[TEST_KEY_UP]) != 0U)
      {
        if (dcIndex < TEST_DC_MAX_INDEX)
        {
          dcIndex++;
        }
        Test_WriteVoltage((float)dcIndex * TEST_DC_STEP_V);
      }
    }
    else
    {
      (void)Test_KeyTakePress(&keys[TEST_KEY_DOWN]);
      (void)Test_KeyTakePress(&keys[TEST_KEY_UP]);
    }

    if (Test_KeyTakePress(&keys[TEST_KEY_MODE]) != 0U)
    {
      Test_SetMode((TestMode_t)((testMode + 1) % TEST_MODE_COUNT), now);
    }

    /* 2. 自动测试序列 */
    Test_UpdateAuto(now);

    /* 3. 刷新屏幕 (自动序列运行时也保持 5 帧/秒) */
    if ((displayDirty != 0U) ||
        ((uint32_t)(now - lastDisplay) >= TEST_DISPLAY_MS))
    {
      Test_Draw();
      OLED_Update();
      displayDirty = 0U;
      lastDisplay = now;
    }

    HAL_Delay(TEST_LOOP_MS);
  }
}

/* ============================ 测试流程 ============================ */

/* 非阻塞按键扫描: 20ms 消抖, 按下确认时产生一次事件 */
static void Test_KeyScan(TestKey_t *key, uint32_t now)
{
  uint8_t raw;

  raw = (HAL_GPIO_ReadPin(key->port, key->pin) == GPIO_PIN_RESET) ? 1U : 0U;
  if (raw != key->raw)
  {
    key->raw = raw;
    key->changedAt = now;
  }

  if (((uint32_t)(now - key->changedAt) >= TEST_KEY_DEBOUNCE_MS) &&
      (raw != key->stable))
  {
    key->stable = raw;
    if (raw != 0U)
    {
      key->pressed = 1U;
    }
  }
}

static uint8_t Test_KeyTakePress(TestKey_t *key)
{
  uint8_t pressed = key->pressed;
  key->pressed = 0U;
  return pressed;
}

static void Test_SetMode(TestMode_t mode, uint32_t now)
{
  testMode = mode;
  phaseStarted = now;
  displayDirty = 1U;

  switch (testMode)
  {
    case TEST_MODE_DC:
      Test_WriteVoltage((float)dcIndex * TEST_DC_STEP_V);
      break;

    case TEST_MODE_STEP:
      stepIndex = 0U;
      Test_WriteCode(stepCodes[stepIndex]);
      break;

    case TEST_MODE_EXTREMES:
      extremeIndex = 0U;
      Test_WriteCode(extremeCodes[extremeIndex]);
      break;

    default:
      powerIndex = 0U;
      /* 先写中值码, 让掉电测试有明确的 1.65V 参考输出 */
      Test_WriteCode(TEST_POWER_CODE);
      break;
  }
}

static void Test_UpdateAuto(uint32_t now)
{
  switch (testMode)
  {
    case TEST_MODE_STEP:
      if ((uint32_t)(now - phaseStarted) >= TEST_STEP_HOLD_MS)
      {
        stepIndex = (uint8_t)((stepIndex + 1U) % TEST_STEP_COUNT);
        Test_WriteCode(stepCodes[stepIndex]);
        phaseStarted = now;
      }
      break;

    case TEST_MODE_EXTREMES:
      if ((uint32_t)(now - phaseStarted) >= TEST_EXTREME_HOLD_MS)
      {
        extremeIndex = (uint8_t)((extremeIndex + 1U) % TEST_EXTREME_COUNT);
        Test_WriteCode(extremeCodes[extremeIndex]);
        phaseStarted = now;
      }
      break;

    case TEST_MODE_POWER:
      if ((uint32_t)(now - phaseStarted) >= TEST_POWER_HOLD_MS)
      {
        powerIndex = (uint8_t)((powerIndex + 1U) % 4U);
        Test_WritePowerDown((AD5662_PowerDown_t)powerIndex);
        phaseStarted = now;
      }
      break;

    default:
      break;
  }
}

static void Test_WriteCode(uint16_t code)
{
  lastStatus = AD5662_WriteCode(&dac, code);
  if (lastStatus != HAL_OK)
  {
    spiFailCount++;
  }
  displayDirty = 1U;
}

static void Test_WriteVoltage(float voltage)
{
  lastStatus = AD5662_SetVoltage(&dac, voltage);
  if (lastStatus != HAL_OK)
  {
    spiFailCount++;
  }
  displayDirty = 1U;
}

static void Test_WritePowerDown(AD5662_PowerDown_t mode)
{
  lastStatus = AD5662_SetPowerDown(&dac, mode);
  if (lastStatus != HAL_OK)
  {
    spiFailCount++;
  }
  displayDirty = 1U;
}

/* ============================ 显示 ============================ */

static void Test_Draw(void)
{
  uint16_t code = AD5662_GetCode(&dac);
  float voltage = AD5662_CodeToVoltage(&dac, code);

  OLED_Clear();

  OLED_ShowString(0, 0, "AD5662 TEST", OLED_6X8);

  OLED_ShowString(0, 8, "MODE", OLED_6X8);
  OLED_ShowNum(24, 8, (uint32_t)testMode + 1U, 1U, OLED_6X8);
  OLED_ShowString(30, 8, "/4", OLED_6X8);
  OLED_ShowString(42, 8, Test_ModeName(testMode), OLED_6X8);

  switch (testMode)
  {
    case TEST_MODE_DC:
      OLED_ShowString(0, 16, "TGT", OLED_6X8);
      OLED_ShowFloatNum(18, 16, (double)dcIndex * TEST_DC_STEP_V, 1U, 2U, OLED_6X8);
      OLED_ShowChar(48, 16, 'V', OLED_6X8);
      OLED_ShowString(60, 16, "STEP 0.1V", OLED_6X8);
      break;

    case TEST_MODE_STEP:
      OLED_ShowString(0, 16, "STEP", OLED_6X8);
      OLED_ShowNum(24, 16, (uint32_t)stepIndex + 1U, 2U, OLED_6X8);
      OLED_ShowChar(36, 16, '/', OLED_6X8);
      OLED_ShowNum(42, 16, (uint32_t)TEST_STEP_COUNT, 2U, OLED_6X8);
      OLED_ShowString(60, 16, "HOLD.75s", OLED_6X8);
      break;

    case TEST_MODE_EXTREMES:
      OLED_ShowString(0, 16, "EXT", OLED_6X8);
      OLED_ShowNum(18, 16, (uint32_t)extremeIndex + 1U, 1U, OLED_6X8);
      OLED_ShowString(24, 16, "/3", OLED_6X8);
      OLED_ShowString(42, 16, "HOLD 1.5s", OLED_6X8);
      break;

    default:
      OLED_ShowString(0, 16, "PD", OLED_6X8);
      OLED_ShowString(18, 16, Test_PowerName(powerIndex), OLED_6X8);
      OLED_ShowString(78, 16, "HOLD 2s", OLED_6X8);
      break;
  }

  OLED_ShowString(0, 24, "CODE", OLED_6X8);
  OLED_ShowNum(24, 24, code, 5U, OLED_6X8);
  OLED_ShowString(54, 24, "0x", OLED_6X8);
  OLED_ShowHexNum(66, 24, code, 4U, OLED_6X8);

  /* 掉电时输出级已断开, 这里的 1.65V 只是 DAC 寄存器保持的码值 */
  OLED_ShowString(0, 32, (testMode == TEST_MODE_POWER) ? "REG" : "DAC", OLED_6X8);
  OLED_ShowFloatNum(18, 32, (double)voltage, 1U, 4U, OLED_6X8);
  OLED_ShowChar(60, 32, 'V', OLED_6X8);

  OLED_ShowString(0, 40, "SPI", OLED_6X8);
  OLED_ShowString(18, 40, (lastStatus == HAL_OK) ? "OK" : "ERR", OLED_6X8);
  OLED_ShowString(42, 40, "ERRCNT", OLED_6X8);
  OLED_ShowNum(78, 40, spiFailCount, 3U, OLED_6X8);

  OLED_ShowString(0, 48, "K1- K2+ K3:NEXT", OLED_6X8);
  OLED_ShowString(0, 56, "MOS OFF 9M CPHA1", OLED_6X8);
}

static char *Test_ModeName(TestMode_t mode)
{
  switch (mode)
  {
    case TEST_MODE_DC:
      return "DC MANUAL";
    case TEST_MODE_STEP:
      return "STEP";
    case TEST_MODE_EXTREMES:
      return "EXTREMES";
    default:
      return "POWERDOWN";
  }
}

static char *Test_PowerName(uint8_t index)
{
  switch (index)
  {
    case 0U:
      return "NORMAL";
    case 1U:
      return "1K GND";
    case 2U:
      return "100K GND";
    default:
      return "HI-Z";
  }
}

/* ============================ 初始化 ============================ */

/**
  * @brief System Clock Configuration (HSE 8MHz x9 = 72MHz, APB1 36MHz)
  */
static void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

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

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                              | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief GPIO 初始化
  *        PB10 输出 MOS / PA2 蜂鸣器保持低电平; PB12 (SYNC) 空闲高.
  *        K1(PA8) / K2(PB11) / K3(PB9) 上拉输入, 按下为低.
  *        PA4/PA5 (OLED) 由 OLED_Init() 自行配置.
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_RESET);   /* 蜂鸣器关 */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);  /* 输出 MOS 关 */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_SET);    /* SYNC 空闲高 */

  GPIO_InitStruct.Pin = GPIO_PIN_2;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = GPIO_PIN_10 | GPIO_PIN_12;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = GPIO_PIN_9 | GPIO_PIN_11;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}

/**
  * @brief SPI2 初始化 (AD5662: 主机、只发、MSB 先行、CPOL=0/CPHA=1)
  *        PCLK1 = 36MHz, 4 分频 = 9MHz, 低于 AD5662 在 3.3V 下的 20MHz 上限.
  */
static void MX_SPI2_Init(void)
{
  hspi2.Instance = SPI2;
  hspi2.Init.Mode = SPI_MODE_MASTER;
  hspi2.Init.Direction = SPI_DIRECTION_1LINE;
  hspi2.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi2.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi2.Init.CLKPhase = TEST_SPI_PHASE;
  hspi2.Init.NSS = SPI_NSS_SOFT;
  hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
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
