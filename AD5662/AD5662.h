/**
  ******************************************************************************
  * @file    AD5662.h
  * @brief   AD5662 (16 位 nanoDAC, 3 线 SPI) 驱动.
  *
  *  硬件连接:
  *    STM32            AD5662
  *    PB13 (SPI2_SCK)  -> SCLK
  *    PB15 (SPI2_MOSI) -> DIN
  *    PB12 (GPIO 输出) -> SYNC   (低有效片选)
  *    VREF             -> 3.3V   (本板 V_Ref)
  *
  *  CubeMX 中 SPI2 需配置为:
  *    Mode              : Transmit Only Master (只需 MOSI, 不占用 PB14)
  *    Data Size         : 8 Bits
  *    First Bit         : MSB First
  *    Clock Polarity    : Low          \
  *    Clock Phase       : 1 Edge        > 即 SPI Mode 1, 器件在下降沿锁存 DIN
  *    NSS               : Software
  *    Baud Rate         : Prescaler 4 -> PCLK1(36MHz)/4 = 9MHz, 低于 20MHz 上限
  *
  *  !! 上述参数尚未全部存进 YLD_Cup.ioc (IPParameters 里缺 CLKPolarity /
  *     CLKPhase / DataSize / FirstBit / NSS / BaudRatePrescaler), 且 .ioc 的
  *     SPI2.Direction 与 PB13/PB15 的 TX_Only 引脚模式自相矛盾.
  *     用 CubeMX 重新生成代码前, 必须先按上表把 SPI2 重配一遍, 否则
  *     MX_SPI2_Init() 会被按 .ioc 覆盖, CPOL/CPHA 一旦回到默认就会整体错位.
  *
  *  数据帧 (24 位, MSB 先行):
  *    DB23 .. DB18 : 无关位
  *    DB17 / DB16  : PD1 / PD0  掉电模式选择
  *    DB15 .. DB0  : 16 位 DAC 数据
  *    第 24 个 SCLK 下降沿把整帧写入 DAC 寄存器, SYNC 上升沿锁存.
  *
  *  传递函数 (增益为 1, 输出轨到轨):
  *    VOUT = VREF * D / 65536      D = 0 .. 65535
  *    VREF = 3.3V 时 1 LSB = 3.3 / 65536 ≈ 50.35 uV
  *
  *  注意 1: AD5662 是只写器件, 没有 SDO 回读引脚.
  *
  *  注意 2: 三个写函数 (WriteCode / SetVoltage / SetPowerDown) 不可重入.
  *          AD5662_WriteFrame 的 "SYNC 拉低 -> SPI 传输 -> SYNC 拉高" 不是原子
  *          序列, 若在中断和主循环里同时调用, 两帧会交错, SYNC 提前拉高会被
  *          器件判为中断帧而整帧丢弃 (见下面 "SYNC 必须包住整帧" 一节).
  *          需要多上下文调用时, 请自行加临界区.
  *          AD5662_VoltageToCode / AD5662_CodeToVoltage 不碰总线, 可安全用于中断.
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
/* 不用 __AD5662_H: 双下划线开头的标识符是 C 标准保留给实现的 (C11 7.1.3) */
#ifndef AD5662_H
#define AD5662_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f1xx_hal.h"

/* Exported types ------------------------------------------------------------*/

/**
  * @brief  掉电模式, 对应 24 位帧里的 PD1/PD0.
  * @note   进入掉电后 DAC 寄存器内容保持不变; 再次写入新码值 (PD=00) 即恢复正常工作.
  */
typedef enum
{
  AD5662_PD_NORMAL      = 0x0,  /*!< 00: 正常工作, 输出接放大器 */
  AD5662_PD_1K_TO_GND   = 0x1,  /*!< 01: 输出经 1kΩ 到 GND   */
  AD5662_PD_100K_TO_GND = 0x2,  /*!< 10: 输出经 100kΩ 到 GND */
  AD5662_PD_TRISTATE    = 0x3   /*!< 11: 输出三态             */
} AD5662_PowerDown_t;

/**
  * @brief  AD5662 设备句柄.
  * @note   一个句柄对应一片 AD5662. 多片时各建一个句柄, 复用同一条 SPI
  *         (SCLK/DIN 并联), 各自占用一根 SYNC 引脚.
  */
typedef struct
{
  SPI_HandleTypeDef *hspi;       /*!< 已初始化的 SPI 句柄, 例如 &hspi2          */
  GPIO_TypeDef      *sync_port;  /*!< SYNC 所在端口, 例如 GPIOB                 */
  uint16_t           sync_pin;   /*!< SYNC 引脚, 例如 GPIO_PIN_12               */
  float              vref;       /*!< 参考电压, 单位 V, 本板为 3.3f             */
  uint16_t           code;       /*!< 最近一次写入的 DAC 码值                   */
  AD5662_PowerDown_t power_down; /*!< 当前掉电模式                              */
} AD5662_t;

/* Exported constants --------------------------------------------------------*/

/** 16 位 DAC 满量程 (码值个数), 传递函数的分母 */
#define AD5662_FULL_SCALE   65536.0f
/** 最大码值 */
#define AD5662_MAX_CODE     0xFFFFU
/** 单帧传输超时, 单位 ms. 9MHz 下 3 字节约 2.7us, 10ms 足够宽松 */
#ifndef AD5662_SPI_TIMEOUT_MS
#define AD5662_SPI_TIMEOUT_MS  10U
#endif

/* Exported functions prototypes ---------------------------------------------*/

/**
  * @brief  初始化驱动并让输出归零.
  * @note   dev / hspi / sync_port 任一为 NULL 时直接返回, 不做任何事.
  *         SYNC 引脚需已在 CubeMX 里配成 GPIO_MODE_OUTPUT_PP.
  *         函数内部会显式写入 0 码, 因此无论器件是 AD5662-1 (上电复位到零刻度)
  *         还是 AD5662-2 (上电复位到中间刻度), 上电输出都是 0V.
  *         若不需要这个行为, 删掉函数末尾的 AD5662_WriteCode 调用即可.
  * @param  dev       : 设备句柄
  * @param  hspi      : 已初始化的 SPI 句柄, 例如 &hspi2
  * @param  sync_port : SYNC 端口, 例如 GPIOB
  * @param  sync_pin  : SYNC 引脚, 例如 GPIO_PIN_12
  * @param  vref      : 参考电压, 单位 V
  * @retval 无
  */
void AD5662_Init(AD5662_t *dev, SPI_HandleTypeDef *hspi, GPIO_TypeDef *sync_port,
                 uint16_t sync_pin, float vref);

/**
  * @brief  直接写入 16 位 DAC 码值 (正常工作模式).
  * @param  dev  : 设备句柄
  * @param  code : 0 .. 65535
  * @retval HAL_OK 表示 SPI 传输成功; 其余值为 HAL 错误码.
  */
HAL_StatusTypeDef AD5662_WriteCode(AD5662_t *dev, uint16_t code);

/**
  * @brief  设定输出电压.
  * @note   输出 = vref * code / 65536, 采用四舍五入取码.
  *         电压 <= 0 取 0 码; 电压 >= vref 取满量程码 (输出无法超过 VREF).
  * @param  dev     : 设备句柄
  * @param  voltage : 目标电压, 单位 V
  * @retval HAL_OK 表示 SPI 传输成功; 其余值为 HAL 错误码.
  */
HAL_StatusTypeDef AD5662_SetVoltage(AD5662_t *dev, float voltage);

/**
  * @brief  设置掉电模式, 保持当前 DAC 码值.
  * @note   传 AD5662_PD_NORMAL 即恢复输出. 也可直接调用 AD5662_SetVoltage 唤醒,
  *         因为写入新码值时 PD 位默认为 00.
  * @param  dev  : 设备句柄
  * @param  mode : 掉电模式
  * @retval HAL_OK 表示 SPI 传输成功; 其余值为 HAL 错误码.
  */
HAL_StatusTypeDef AD5662_SetPowerDown(AD5662_t *dev, AD5662_PowerDown_t mode);

/**
  * @brief  电压 -> 码值换算 (不发起传输).
  * @note   dev 为 NULL, 或 vref / voltage 非法 (<= 0 或 NaN) 时返回 0.
  * @param  dev     : 设备句柄
  * @param  voltage : 目标电压, 单位 V
  * @retval 0 .. 65535
  */
uint16_t AD5662_VoltageToCode(const AD5662_t *dev, float voltage);

/**
  * @brief  码值 -> 电压换算 (不发起传输).
  * @param  dev  : 设备句柄
  * @param  code : 0 .. 65535
  * @retval 该码值对应的理论输出电压, 单位 V
  */
float AD5662_CodeToVoltage(const AD5662_t *dev, uint16_t code);

/**
  * @brief  取回最近一次成功写入的码值 (器件本身不可回读).
  * @param  dev : 设备句柄
  * @retval 0 .. 65535
  */
uint16_t AD5662_GetCode(const AD5662_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* AD5662_H */
