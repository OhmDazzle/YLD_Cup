/**
  ******************************************************************************
  * @file    oled_test_main.c
  * @brief   独立 OLED 测试固件 (TEST_OLED/): 单独编译、单独烧录.
  *
  *  本文件自成体系: 自带 main()、时钟与 GPIO 初始化, 不依赖 Core/Src/main.c,
  *  只复用板级文件 (启动/中断/MSP) 和 OLED 驱动.
  *  构建目标: YLD_Cup_OLEDTest (见 TEST_OLED/CMakeLists.txt).
  *
  *  屏幕为 0.96 寸 128x64, 软件 I2C: SCL=PA4, SDA=PA5.
  *  六个测试页循环显示, 用于检查坏点、缺行/缺列、对比度与刷屏速度:
  *    0 文字/中文字模   1 单像素网格     2 1px/2px 条纹
  *    3 灰度密度图      4 图形与直线     5 动画与进度条
  *
  *  按键: K1(PA8) 上一页, K2(PB11) 下一页,
  *        K3(PB9) 短按切换自动翻页, 长按切换蜂鸣器 (PA2 高电平=响).
  ******************************************************************************
  */

#include <stdint.h>
#include "stm32g4xx_hal.h"
#include "OLED.h"

/* ---- 本固件的可调参数 ---- */
#define TEST_PAGE_COUNT       6U
#define TEST_LOOP_MS          10U     /* 主循环节拍 */
#define TEST_AUTO_ADVANCE_MS  3000U   /* 自动翻页间隔 */
#define TEST_ANIM_FRAME_MS    50U     /* 动画页刷新间隔 */
#define TEST_KEY_DEBOUNCE_MS  20U     /* 按键消抖时间 */
#define TEST_KEY_LONG_MS      800U    /* 长按判定时间 */

/* 按键索引 */
#define TEST_KEY_PREV         0U
#define TEST_KEY_NEXT         1U
#define TEST_KEY_AUTO         2U

/* ---- 按键事件 ---- */
typedef enum
{
  TEST_KEY_EV_NONE = 0,
  TEST_KEY_EV_SHORT,        /* 松手时产生, 且本次没有触发过长按 */
  TEST_KEY_EV_LONG,         /* 按住达到 TEST_KEY_LONG_MS, 只产生一次 */
} TestKeyEvent_t;

/* ---- 外设/按键对象 ---- */
typedef struct
{
  GPIO_TypeDef *port;
  uint16_t      pin;
  uint8_t       rawLast;      /* 上次读到的原始电平: 1=按下 */
  uint8_t       stable;       /* 消抖后的电平: 1=按下 */
  uint8_t       longEnabled;  /* 该键是否启用长按事件 */
  uint8_t       longFired;    /* 本次按住已产生过长按事件 */
  uint32_t      changedAt;    /* 原始电平最后一次变化的时刻 */
  uint32_t      pressedAt;    /* 消抖确认按下的时刻 */
  TestKeyEvent_t event;       /* 待处理的按键事件 */
} TestKey_t;

static TestKey_t keys[3] =
{
  { .port = GPIOA, .pin = GPIO_PIN_8  },                    /* K1: 上一页 */
  { .port = GPIOB, .pin = GPIO_PIN_11 },                    /* K2: 下一页 */
  { .port = GPIOB, .pin = GPIO_PIN_9, .longEnabled = 1U },  /* K3: 短按/长按 */
};

/* ---- 测试状态 ---- */
static uint8_t  pageIndex = 0U;
static uint8_t  autoAdvance = 1U;
static uint8_t  buzzerOn = 0U;
static uint8_t  pageDirty = 1U;
static uint32_t lastPageMs = 0U;
static uint32_t lastAnimMs = 0U;
static uint32_t animFrame = 0U;
static int16_t  animX = 2;
static int16_t  animDir = 1;

/* ---- 内部函数声明 ---- */
static void Error_Handler(void);
static void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void Test_KeyScan(TestKey_t *key, uint32_t now);
static TestKeyEvent_t Test_KeyTakeEvent(TestKey_t *key);
static void Test_BuzzerSet(uint8_t on);
static void Test_ShowPageNumber(uint8_t page);
static void Test_DrawWelcome(void);
static void Test_DrawPixelGrid(void);
static void Test_DrawStripes(void);
static void Test_DrawDensity(void);
static void Test_DrawShapes(void);
static void Test_DrawAnimation(void);
static void Test_DrawPage(uint8_t page);

/* ============================ 主函数 ============================ */

int main(void)
{
  uint32_t now;
  TestKeyEvent_t keyEvent;

  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  Test_BuzzerSet(0U);

  /* 屏幕先做一次全白 -> 全黑闪屏, 上电即可确认背光/供电正常 */
  OLED_Init();
  OLED_DrawRectangle(0, 0, 128, 64, OLED_FILLED);
  OLED_Update();
  HAL_Delay(400);
  OLED_Clear();
  OLED_Update();
  HAL_Delay(200);

  lastPageMs = HAL_GetTick();
  lastAnimMs = lastPageMs;

  while (1)
  {
    now = HAL_GetTick();

    /* 1. 按键扫描: 上一页 / 下一页 / K3 短按自动翻页、长按蜂鸣器 */
    Test_KeyScan(&keys[TEST_KEY_PREV], now);
    Test_KeyScan(&keys[TEST_KEY_NEXT], now);
    Test_KeyScan(&keys[TEST_KEY_AUTO], now);

    if (Test_KeyTakeEvent(&keys[TEST_KEY_PREV]) != TEST_KEY_EV_NONE)
    {
      pageIndex = (uint8_t)((pageIndex + TEST_PAGE_COUNT - 1U) % TEST_PAGE_COUNT);
      pageDirty = 1U;
      lastPageMs = now;
    }
    if (Test_KeyTakeEvent(&keys[TEST_KEY_NEXT]) != TEST_KEY_EV_NONE)
    {
      pageIndex = (uint8_t)((pageIndex + 1U) % TEST_PAGE_COUNT);
      pageDirty = 1U;
      lastPageMs = now;
    }
    keyEvent = Test_KeyTakeEvent(&keys[TEST_KEY_AUTO]);
    if (keyEvent == TEST_KEY_EV_SHORT)
    {
      autoAdvance = (autoAdvance == 0U) ? 1U : 0U;
      pageDirty = 1U;
      lastPageMs = now;
    }
    else if (keyEvent == TEST_KEY_EV_LONG)
    {
      buzzerOn = (buzzerOn == 0U) ? 1U : 0U;
      Test_BuzzerSet(buzzerOn);
      pageDirty = 1U;
    }

    /* 2. 自动翻页 */
    if ((autoAdvance != 0U) && ((uint32_t)(now - lastPageMs) >= TEST_AUTO_ADVANCE_MS))
    {
      pageIndex = (uint8_t)((pageIndex + 1U) % TEST_PAGE_COUNT);
      pageDirty = 1U;
      lastPageMs = now;
    }

    /* 3. 动画页持续刷新, 其余页面只在切换时重绘 */
    if (pageIndex == 5U && (uint32_t)(now - lastAnimMs) >= TEST_ANIM_FRAME_MS)
    {
      animFrame++;
      animX = (int16_t)(animX + animDir * 2);
      if (animX >= 110)
      {
        animX = 110;
        animDir = -1;
      }
      else if (animX <= 2)
      {
        animX = 2;
        animDir = 1;
      }
      pageDirty = 1U;
      lastAnimMs = now;
    }

    if (pageDirty != 0U)
    {
      Test_DrawPage(pageIndex);
      OLED_Update();
      pageDirty = 0U;
    }

    HAL_Delay(TEST_LOOP_MS);
  }
}

/* ============================ 测试页面 ============================ */

/* 右上角页码 "x/6" */
static void Test_ShowPageNumber(uint8_t page)
{
  OLED_ShowNum(110, 0, (uint32_t)(page + 1U), 1U, OLED_6X8);
  OLED_ShowChar(116, 0, '/', OLED_6X8);
  OLED_ShowNum(122, 0, TEST_PAGE_COUNT, 1U, OLED_6X8);
}

/* 第 0 页: 8x16 ASCII、6x8 ASCII、16x16 汉字与边框 */
static void Test_DrawWelcome(void)
{
  OLED_Clear();
  OLED_DrawRectangle(0, 0, 128, 64, OLED_UNFILLED);

  OLED_ShowString(28, 2, "OLED TEST", OLED_8X16);
  OLED_ShowString(22, 19, "YLD Cup 128x64", OLED_6X8);
  OLED_ShowString(32, 28, "你好世界", OLED_8X16);

  if (autoAdvance != 0U)
  {
    OLED_ShowString(10, 46, "S:AUTO ON K1/K2 PG", OLED_6X8);
  }
  else
  {
    OLED_ShowString(7, 46, "S:AUTO OFF K1/K2 PG", OLED_6X8);
  }
  OLED_ShowString(28, 54, (buzzerOn != 0U) ? "L:K3 BUZ ON" : "L:K3 BUZ OFF", OLED_6X8);
}

/* 第 1 页: 单像素点阵 + 1px 边框, 便于检查坏点与列/行缺失 */
static void Test_DrawPixelGrid(void)
{
  int16_t x, y;
  int16_t offset;

  OLED_Clear();
  OLED_ShowString(0, 0, "PIXEL GRID", OLED_6X8);
  Test_ShowPageNumber(1U);

  for (x = 0; x < 128; x++)
  {
    OLED_DrawPoint(x, 0);
    OLED_DrawPoint(x, 63);
  }
  for (y = 0; y < 64; y++)
  {
    OLED_DrawPoint(0, y);
    OLED_DrawPoint(127, y);
  }

  /* 交错点阵: 相邻两行错开 2px, 每个点只占 1 个像素 */
  for (y = 12; y < 62; y = (int16_t)(y + 4))
  {
    offset = (((y / 4) & 1) != 0) ? 2 : 0;
    for (x = (int16_t)(2 + offset); x < 126; x = (int16_t)(x + 4))
    {
      OLED_DrawPoint(x, y);
    }
  }
}

/* 第 2 页: 1px / 2px 垂直、水平条纹与棋盘格 */
static void Test_DrawStripes(void)
{
  int16_t x, y;
  uint8_t on;

  OLED_Clear();
  OLED_ShowString(0, 0, "STRIPES", OLED_6X8);
  Test_ShowPageNumber(2U);

  for (y = 8; y < 64; y++)
  {
    for (x = 0; x < 128; x++)
    {
      if (y < 20)
      {
        on = ((x & 1) != 0) ? 1U : 0U;              /* 垂直 1px */
      }
      else if (y < 32)
      {
        on = (((x / 2) & 1) != 0) ? 1U : 0U;        /* 垂直 2px */
      }
      else if (y < 44)
      {
        on = ((y & 1) != 0) ? 1U : 0U;              /* 水平 1px */
      }
      else if (y < 56)
      {
        on = (((y / 2) & 1) != 0) ? 1U : 0U;        /* 水平 2px */
      }
      else
      {
        on = (((x + y) & 1) != 0) ? 1U : 0U;        /* 1px 棋盘格 */
      }

      if (on != 0U)
      {
        OLED_DrawPoint(x, y);
      }
    }
  }
}

/* 第 3 页: 0% / 25% / 50% / 75% / 100% 五档密度 */
static void Test_DrawDensity(void)
{
  int16_t x, y;
  uint8_t band;
  uint8_t on;

  OLED_Clear();
  OLED_ShowString(0, 0, "DENSITY", OLED_6X8);
  Test_ShowPageNumber(3U);

  for (x = 0; x < 128; x++)
  {
    band = (uint8_t)(((uint16_t)x * 5U) / 128U);     /* 0~4 */
    for (y = 8; y < 64; y++)
    {
      switch (band)
      {
        case 0U:
          on = 0U;                                    /* 0% */
          break;
        case 1U:
          on = (((x & 1) == 0) && ((y & 1) == 0)) ? 1U : 0U;   /* 25% */
          break;
        case 2U:
          on = (((x + y) & 1) == 0) ? 1U : 0U;         /* 50% */
          break;
        case 3U:
          on = (((x & 1) == 0) || ((y & 1) == 0)) ? 1U : 0U;   /* 75% */
          break;
        default:
          on = 1U;                                    /* 100% */
          break;
      }

      if (on != 0U)
      {
        OLED_DrawPoint(x, y);
      }
    }
  }
}

/* 第 4 页: 直线、矩形、圆、实心圆与三角形 */
static void Test_DrawShapes(void)
{
  OLED_Clear();
  OLED_ShowString(0, 0, "SHAPES", OLED_6X8);
  Test_ShowPageNumber(4U);

  OLED_DrawLine(4, 12, 60, 60);
  OLED_DrawLine(60, 12, 4, 60);
  OLED_DrawRectangle(8, 21, 48, 26, OLED_UNFILLED);

  OLED_DrawCircle(96, 27, 13, OLED_UNFILLED);
  OLED_DrawCircle(96, 27, 5, OLED_FILLED);
  OLED_DrawTriangle(68, 60, 124, 60, 96, 40, OLED_UNFILLED);
}

/* 第 5 页: 移动方块 + 进度条, 检查连续刷屏与动态效果 */
static void Test_DrawAnimation(void)
{
  uint8_t fillWidth;

  OLED_Clear();
  OLED_DrawRectangle(0, 0, 128, 64, OLED_UNFILLED);
  OLED_ShowString(28, 2, "ANIMATION", OLED_6X8);
  Test_ShowPageNumber(5U);

  OLED_DrawRectangle(animX, 20, 16, 12, OLED_FILLED);

  OLED_ShowString(6, 38, "K1 PREV K2 NEXT", OLED_6X8);
  if (autoAdvance != 0U)
  {
    OLED_ShowString((buzzerOn != 0U) ? 10 : 7, 46,
                    (buzzerOn != 0U) ? "S:AUTO ON L:BUZ ON" : "S:AUTO ON L:BUZ OFF",
                    OLED_6X8);
  }
  else
  {
    OLED_ShowString((buzzerOn != 0U) ? 7 : 4, 46,
                    (buzzerOn != 0U) ? "S:AUTO OFF L:BUZ ON" : "S:AUTO OFF L:BUZ OFF",
                    OLED_6X8);
  }

  OLED_DrawRectangle(6, 55, 116, 7, OLED_UNFILLED);
  fillWidth = (uint8_t)(((animFrame % 100U) * 112U) / 100U);
  if (fillWidth != 0U)
  {
    OLED_DrawRectangle(8, 57, fillWidth, 3, OLED_FILLED);
  }
}

static void Test_DrawPage(uint8_t page)
{
  switch (page)
  {
    case 0U:
      Test_DrawWelcome();
      break;
    case 1U:
      Test_DrawPixelGrid();
      break;
    case 2U:
      Test_DrawStripes();
      break;
    case 3U:
      Test_DrawDensity();
      break;
    case 4U:
      Test_DrawShapes();
      break;
    default:
      Test_DrawAnimation();
      break;
  }
}

/* ============================ 按键与初始化 ============================ */

/* 非阻塞按键扫描: 20ms 消抖; 短按在松手时产生, 长按按住到时限时产生一次 */
static void Test_KeyScan(TestKey_t *key, uint32_t now)
{
  uint8_t raw;

  raw = (HAL_GPIO_ReadPin(key->port, key->pin) == GPIO_PIN_RESET) ? 1U : 0U;
  if (raw != key->rawLast)
  {
    key->rawLast = raw;
    key->changedAt = now;
  }

  if (((uint32_t)(now - key->changedAt) >= TEST_KEY_DEBOUNCE_MS) &&
      (raw != key->stable))
  {
    key->stable = raw;
    if (raw != 0U)
    {
      key->pressedAt = now;
      key->longFired = 0U;
    }
    else
    {
      /* 已经产生过长按事件, 松手时不再补发短按 */
      if ((key->longEnabled == 0U) || (key->longFired == 0U))
      {
        key->event = TEST_KEY_EV_SHORT;
      }
      key->longFired = 0U;
    }
  }

  if ((key->longEnabled != 0U) && (key->stable != 0U) && (key->longFired == 0U) &&
      ((uint32_t)(now - key->pressedAt) >= TEST_KEY_LONG_MS))
  {
    key->longFired = 1U;
    key->event = TEST_KEY_EV_LONG;
  }
}

static TestKeyEvent_t Test_KeyTakeEvent(TestKey_t *key)
{
  TestKeyEvent_t event = key->event;
  key->event = TEST_KEY_EV_NONE;
  return event;
}

/* 蜂鸣器 MOS: PA2 高电平导通 */
static void Test_BuzzerSet(uint8_t on)
{
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, (on != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/**
  * @brief GPIO 初始化
  *        按键 K1/K2/K3 上拉输入; 同时把输出 MOS、蜂鸣器拉低,
  *        SYNC 置高, 避免测试固件运行时误开输出。
  *        PB3/PB4 (OLED) 由 OLED_Init() 自行配置。
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1 | GPIO_PIN_2, GPIO_PIN_RESET); /* MOS/蜂鸣器关 */

  /* PA1: 输出 MOS; PA2: 蜂鸣器; 都先拉低 */
  GPIO_InitStruct.Pin = GPIO_PIN_1 | GPIO_PIN_2;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* 按键: A8 / B11 / B9 上拉输入 (按下=低) */
  GPIO_InitStruct.Pin = GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = GPIO_PIN_11 | GPIO_PIN_9;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* OLED SCL/SDA (PB3/PB4) 由 OLED_Init() 配成开漏 */
}

/**
  * @brief System Clock Configuration (HSE 8MHz -> PLL 170MHz)
  */
static void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

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

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                              | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
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
  * @brief 出错处理: 关中断并停住
  */
static void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}
