/**
  ******************************************************************************
  * @file    AD5662.c
  * @brief   AD5662 (16 位 nanoDAC, 3 线 SPI) 驱动实现.
  *
  *  用法示例 (main.c):
  *    #include "AD5662.h"
  *
  *    static AD5662_t dac;
  *
  *    // MX_SPI2_Init() 之后:
  *    AD5662_Init(&dac, &hspi2, GPIOB, GPIO_PIN_12, 3.3f);
  *
  *    AD5662_SetVoltage(&dac, 1.5f);          // 输出 1.5V
  *    AD5662_SetPowerDown(&dac, AD5662_PD_TRISTATE);
  *    AD5662_SetVoltage(&dac, 20.0f);         // 超量程, 钳到满量程 3.3V
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "AD5662.h"

/* Private function prototypes -----------------------------------------------*/
static HAL_StatusTypeDef AD5662_WriteFrame(AD5662_t *dev, uint16_t code,
                                          AD5662_PowerDown_t mode);

/* Exported functions --------------------------------------------------------*/

/**
  * @brief  初始化驱动并让输出归零.
  */
void AD5662_Init(AD5662_t *dev, SPI_HandleTypeDef *hspi, GPIO_TypeDef *sync_port,
                 uint16_t sync_pin, float vref)
{
  if (dev == NULL)
  {
    return;
  }

  dev->hspi      = hspi;
  dev->sync_port = sync_port;
  dev->sync_pin  = sync_pin;
  dev->vref      = vref;
  dev->code      = 0U;
  dev->power_down = AD5662_PD_NORMAL;

  /* 帧间与上电后 SYNC 保持高电平, 否则移位寄存器会一直处于接收状态 */
  HAL_GPIO_WritePin(dev->sync_port, dev->sync_pin, GPIO_PIN_SET);

  /* 显式写 0 码, 让 AD5662-1 / -2 两种上电复位行为统一为输出 0V */
  (void)AD5662_WriteCode(dev, 0U);
}

/**
  * @brief  直接写入 16 位 DAC 码值 (正常工作模式).
  */
HAL_StatusTypeDef AD5662_WriteCode(AD5662_t *dev, uint16_t code)
{
  if (dev == NULL)
  {
    return HAL_ERROR;
  }

  return AD5662_WriteFrame(dev, code, AD5662_PD_NORMAL);
}

/**
  * @brief  设定输出电压.
  */
HAL_StatusTypeDef AD5662_SetVoltage(AD5662_t *dev, float voltage)
{
  if (dev == NULL)
  {
    return HAL_ERROR;
  }

  return AD5662_WriteFrame(dev, AD5662_VoltageToCode(dev, voltage),
                           AD5662_PD_NORMAL);
}

/**
  * @brief  设置掉电模式, 保持当前 DAC 码值.
  */
HAL_StatusTypeDef AD5662_SetPowerDown(AD5662_t *dev, AD5662_PowerDown_t mode)
{
  if (dev == NULL)
  {
    return HAL_ERROR;
  }

  return AD5662_WriteFrame(dev, dev->code, mode);
}

/**
  * @brief  电压 -> 码值换算.
  */
uint16_t AD5662_VoltageToCode(const AD5662_t *dev, float voltage)
{
  float code;

  /* 参考电压非法时无法换算, 只能给 0 */
  if ((dev == NULL) || (dev->vref <= 0.0f))
  {
    return 0U;
  }

  /* 输出轨到轨但无法超出 VREF, 两端直接钳位, 避免下标越界 */
  if (voltage <= 0.0f)
  {
    return 0U;
  }
  if (voltage >= dev->vref)
  {
    return AD5662_MAX_CODE;
  }

  /* 四舍五入取整后再钳一次: 电压略小于 vref 时四舍五入可能得到 65536,
     直接截断成 uint16_t 会溢出回 0 */
  code = voltage * AD5662_FULL_SCALE / dev->vref + 0.5f;
  if (code >= AD5662_FULL_SCALE)
  {
    return AD5662_MAX_CODE;
  }

  return (uint16_t)code;
}

/**
  * @brief  码值 -> 电压换算.
  */
float AD5662_CodeToVoltage(const AD5662_t *dev, uint16_t code)
{
  if (dev == NULL)
  {
    return 0.0f;
  }

  return (float)code * dev->vref / AD5662_FULL_SCALE;
}

/**
  * @brief  取回最近一次成功写入的码值.
  */
uint16_t AD5662_GetCode(const AD5662_t *dev)
{
  if (dev == NULL)
  {
    return 0U;
  }

  return dev->code;
}

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  组一帧 24 位数据并经 SPI2 发给 AD5662.
  * @note   24 位帧按 MSB 先行拆成 3 个字节:
  *           buf[0] = DB23..DB16 (6 个无关位 + PD1 + PD0)
  *           buf[1] = DB15..DB8
  *           buf[2] = DB7 ..DB0
  *         SYNC 必须在整帧期间保持低电平, 第 24 个 SCLK 下降沿写入 DAC 寄存器,
  *         SYNC 上升沿锁存. 因此 SYNC 由 GPIO 手动控制, 不用硬件 NSS
  *         (硬件 NSS 会每字节拉高一次, 那会被器件当成中断帧而丢弃).
  *         SCLK 中途停顿不影响: 帧边界由 SYNC 界定, 不要求时钟连续.
  * @param  dev  : 设备句柄
  * @param  code : 16 位 DAC 码值
  * @param  mode : 掉电模式, 占 PD1/PD0 两位
  * @retval HAL_OK 表示 SPI 传输成功; 其余值为 HAL 错误码.
  */
static HAL_StatusTypeDef AD5662_WriteFrame(AD5662_t *dev, uint16_t code,
                                          AD5662_PowerDown_t mode)
{
  uint8_t        buf[3];
  HAL_StatusTypeDef status;

  buf[0] = (uint8_t)((uint8_t)mode & 0x03U); /* DB23..DB16: 无关位填 0 */
  buf[1] = (uint8_t)(code >> 8);             /* DB15..DB8               */
  buf[2] = (uint8_t)(code & 0x00FFU);        /* DB7 ..DB0               */

  HAL_GPIO_WritePin(dev->sync_port, dev->sync_pin, GPIO_PIN_RESET);

  status = HAL_SPI_Transmit(dev->hspi, buf, 3U, AD5662_SPI_TIMEOUT_MS);

  /* 无论成败都要拉高 SYNC: 传输失败时若把它留在低电平, 器件会一直等待后续
     时钟, 下一帧的前几个边沿就会被错当成上一帧的续传 */
  HAL_GPIO_WritePin(dev->sync_port, dev->sync_pin, GPIO_PIN_SET);

  if (status == HAL_OK)
  {
    dev->code       = code;
    dev->power_down = mode;
  }

  return status;
}
