# AD5662 驱动

STM32F103 + AD5662（16 位 nanoDAC，3 线 SPI）驱动，V_Ref = 3.3V。

- `AD5662.h` — 类型、宏、函数声明
- `AD5662.c` — 实现
- 独立硬件测试固件见 [TEST_AD5662/README.md](../TEST_AD5662/README.md)

---

## 1. 硬件连接

| STM32 | AD5662 | 说明 |
|---|---|---|
| PB13 (SPI2_SCK) | SCLK | 时钟 |
| PB15 (SPI2_MOSI) | DIN | 数据 |
| PB12 (GPIO 输出) | SYNC | 片选，低有效 |
| 3.3V | VREF | 参考电压 |
| — | VFB | 必须接到 VOUT |

AD5662 是**只写**器件，没有 SDO 回读引脚。

---

## 2. 前置配置

### CubeMX

启用 SPI2，参数如下：

| 项 | 值 |
|---|---|
| Mode | Transmit Only Master（只需 MOSI，不占 PB14） |
| Data Size | 8 Bits |
| First Bit | MSB First |
| Clock Polarity | Low |
| Clock Phase | 2 Edge（CPHA=1） |
| NSS | Software |
| Prescaler | 4 → 9 MHz |

CPOL = Low + CPHA = 1（CubeMX 里选 **2 Edge**，HAL 为 `SPI_PHASE_2EDGE`）即 **SPI Mode 1**：SCLK 空闲低，器件在**下降沿**锁存 DIN，手册"微处理器接口"一章的 68HC11 示例明确要求主机 CPOL=0、CPHA=1。注意 CubeMX 的 "1 Edge" 对应 CPHA=0（Mode 0），不是本器件要的配置；当前 `MX_SPI2_Init()` 生成的还停在 1 Edge，待修正（见工程 README 待办）。

9 MHz 低于 AD5662 在 VDD = 2.7~3.6 V 时的 20 MHz 上限（PCLK1 = 36 MHz，4 分频）。

### ⚠️ 重新生成代码前必读

`YLD_Cup.ioc` 与当前代码**仍不完全同步**（2026.10.11 核对）：

- `SPI2.IPParameters` 只记录了 `VirtualType,Mode,Direction,CalculateBaudRate`，**`CLKPolarity` / `CLKPhase` / `DataSize` / `FirstBit` / `NSS` / `BaudRatePrescaler` 都没存进去**，重新生成时这些项会按 CubeMX 默认值填充。
- `ProjectManager.functionlistsort` 里**已有 `MX_SPI2_Init`**，重新生成会正常生成并调用它，但生成的参数不保证与上表一致。
- .ioc 里 `SPI2.Direction=SPI_DIRECTION_2LINES`、`CalculateBaudRate=18.0 MBits/s`（与当前代码的 ÷2 一致），和上表推荐的 Transmit Only / ÷4 不一致；并且 .ioc 的 `SPI2.Direction` 与 `PB13/PB15.Mode=TX_Only_Simplex_Unidirect_Master` 仍**自相矛盾**。

后果：直接在 CubeMX 里点 "Generate Code" 而不先改配置，`MX_SPI2_Init()` 会被按 .ioc + 默认值重新生成——**CPOL/CPHA 一旦不对，输出会整体错位**。

**做法**：在 CubeMX 里按上表把 SPI2 重配一遍再生成，让 .ioc 成为唯一事实来源（Transmit Only / ÷4 9MHz 与当前代码的 2LINES / ÷2 18MHz 二选一，全工程保持一致）。生成后 diff 一下 `MX_SPI2_Init()`，确认参数与选定口径一致。

### 其他检查项

- **PB12 初始电平建议设为 High**（CubeMX 里 GPIO 配置页的 GPIO output level）。当前 `MX_GPIO_Init()` 生成的是 `RESET`，即上电后到 `AD5662_Init()` 执行前 SYNC 一直处于选中状态。目前无害（这段时间没有 SCLK），但 SYNC 空闲高才是正确状态。注意 `HAL_GPIO_WritePin(...GPIO_PIN_RESET)` 这行在生成区，改用户区会被下次生成冲掉。
- SPI2 已启用：`HAL_SPI_MODULE_ENABLED` 已打开（`Core/Inc/stm32f1xx_hal_conf.h`），`stm32f1xx_hal_spi.c` 已在编译列表里（`cmake/stm32cubemx/CMakeLists.txt`）。

### CMakeLists.txt

已加好，无需改动：

```cmake
target_sources(... ${CMAKE_CURRENT_SOURCE_DIR}/AD5662/AD5662.c)
target_include_directories(... ${CMAKE_CURRENT_SOURCE_DIR}/AD5662)
```

---

## 3. 数据结构

### `AD5662_t` — 设备句柄

一个句柄对应一片 AD5662。多片时各建一个句柄，复用同一条 SPI（SCLK/DIN 并联），各自占用一根 SYNC。

| 字段 | 类型 | 说明 |
|---|---|---|
| `hspi` | `SPI_HandleTypeDef *` | 已初始化的 SPI 句柄，例如 `&hspi2` |
| `sync_port` | `GPIO_TypeDef *` | SYNC 所在端口，例如 `GPIOB` |
| `sync_pin` | `uint16_t` | SYNC 引脚，例如 `GPIO_PIN_12` |
| `vref` | `float` | 参考电压，单位 V，本板 `3.3f` |
| `code` | `uint16_t` | 最近一次写入的 DAC 码值 |
| `power_down` | `AD5662_PowerDown_t` | 当前掉电模式 |

### `AD5662_PowerDown_t` — 掉电模式

对应 24 位帧里的 PD1/PD0。进入掉电后 DAC 寄存器内容保持不变，但输出级断开。

| 枚举值 | PD1 PD0 | 行为 |
|---|---|---|
| `AD5662_PD_NORMAL` | 0 0 | 正常工作，输出接放大器 |
| `AD5662_PD_1K_TO_GND` | 0 1 | 输出经 1 kΩ 到 GND |
| `AD5662_PD_100K_TO_GND` | 1 0 | 输出经 100 kΩ 到 GND |
| `AD5662_PD_TRISTATE` | 1 1 | 输出三态 |

### 宏

| 宏 | 值 | 说明 |
|---|---|---|
| `AD5662_FULL_SCALE` | `65536.0f` | 16 位满量程（码值个数），传递函数分母 |
| `AD5662_MAX_CODE` | `0xFFFFU` | 最大码值 65535 |
| `AD5662_SPI_TIMEOUT_MS` | `10U` | 单帧传输超时，单位 ms。可在头文件外重定义 |

---

## 4. 函数列表

### 4.1 `AD5662_Init`

```c
void AD5662_Init(AD5662_t *dev, SPI_HandleTypeDef *hspi, GPIO_TypeDef *sync_port,
                 uint16_t sync_pin, float vref);
```

初始化驱动并让输出归零。

| 参数 | 说明 |
|---|---|
| `dev` | 设备句柄 |
| `hspi` | 已初始化的 SPI 句柄，例如 `&hspi2` |
| `sync_port` | SYNC 端口，例如 `GPIOB` |
| `sync_pin` | SYNC 引脚，例如 `GPIO_PIN_12` |
| `vref` | 参考电压，单位 V |

返回：无。

内部动作：校验三个指针 → 保存配置 → SYNC 拉高（空闲态）→ 显式写入 0 码。显式写 0 是为了让 AD5662-1（上电复位到零刻度）和 AD5662-2（上电复位到中间刻度）两种型号上电行为统一为输出 0V。**不需要这个行为就删掉函数末尾那行 `AD5662_WriteCode`。**

`dev` / `hspi` / `sync_port` 任一为 `NULL` 时直接返回，不做任何事。

`sync_port` / `sync_pin` 需已在 CubeMX 里配成 `GPIO_MODE_OUTPUT_PP`。

---

### 4.2 `AD5662_WriteCode`

```c
HAL_StatusTypeDef AD5662_WriteCode(AD5662_t *dev, uint16_t code);
```

直接写入 16 位 DAC 码值，以正常工作模式（PD = 00）输出。

| 参数 | 说明 |
|---|---|
| `dev` | 设备句柄 |
| `code` | 0 … 65535 |

返回：`HAL_OK` 表示 SPI 传输成功，其余为 HAL 错误码（`HAL_ERROR` / `HAL_BUSY` / `HAL_TIMEOUT`）。

只做原始码值输出，不涉及电压换算，适合需要精确控制码值的场合。写入时 PD 位固定为 00，因此这个函数同时也能把器件从掉电状态唤醒。

---

### 4.3 `AD5662_SetVoltage`

```c
HAL_StatusTypeDef AD5662_SetVoltage(AD5662_t *dev, float voltage);
```

设定输出电压。

| 参数 | 说明 |
|---|---|
| `dev` | 设备句柄 |
| `voltage` | 目标电压，单位 V |

返回：`HAL_OK` 表示 SPI 传输成功，其余为 HAL 错误码。

换算关系 `code = voltage × 65536 / vref`，四舍五入取整。边界处理：

- `voltage <= 0` → 0 码
- `voltage >= vref` → 满量程码 65535（输出无法超过 VREF）

内部先取整后钳位：电压略小于 vref 时四舍五入会算出 65536，若直接截断成 `uint16_t` 会溢出回 0。

---

### 4.4 `AD5662_SetPowerDown`

```c
HAL_StatusTypeDef AD5662_SetPowerDown(AD5662_t *dev, AD5662_PowerDown_t mode);
```

设置掉电模式，**保持当前 DAC 码值**（用 `dev->code` 重新组帧，只改 PD 位）。

| 参数 | 说明 |
|---|---|
| `dev` | 设备句柄 |
| `mode` | 掉电模式，见 `AD5662_PowerDown_t` |

返回：`HAL_OK` 表示 SPI 传输成功，其余为 HAL 错误码。**`mode` 不是 `AD5662_PowerDown_t` 的合法值时返回 `HAL_ERROR`**，且此时不触碰 SYNC、总线上无任何动作。

传 `AD5662_PD_NORMAL` 即恢复输出。也可以直接调 `AD5662_SetVoltage()` 唤醒，因为写新码值时 PD 位默认为 00。

---

### 4.5 `AD5662_VoltageToCode`

```c
uint16_t AD5662_VoltageToCode(const AD5662_t *dev, float voltage);
```

电压 → 码值换算，**不发起 SPI 传输**。

| 参数 | 说明 |
|---|---|
| `dev` | 设备句柄 |
| `voltage` | 目标电压，单位 V |

返回：0 … 65535。`dev == NULL`、`vref` 或 `voltage` 为负或 `NaN` 时返回 0。

钳位规则与 `AD5662_SetVoltage` 一致。两处判断都写的是 `!(x > 0.0f)` 而不是 `x <= 0.0f`，这样 `NaN` 会被一并挡住——`NaN` 与任何值比较都返回假，会一路穿到 `(uint16_t)` 强转，而浮点转无符号整数在值不可表示时是未定义行为（C11 6.3.1.4）。

---

### 4.6 `AD5662_CodeToVoltage`

```c
float AD5662_CodeToVoltage(const AD5662_t *dev, uint16_t code);
```

码值 → 电压换算，**不发起 SPI 传输**。用于反查当前理论输出电压：`AD5662_CodeToVoltage(&dac, AD5662_GetCode(&dac))`。

| 参数 | 说明 |
|---|---|
| `dev` | 设备句柄 |
| `code` | 0 … 65535 |

返回：该码值对应的理论输出电压，单位 V。`dev == NULL` 时返回 `0.0f`。

---

### 4.7 `AD5662_GetCode`

```c
uint16_t AD5662_GetCode(const AD5662_t *dev);
```

取回最近一次**成功写入**的码值。

| 参数 | 说明 |
|---|---|
| `dev` | 设备句柄 |

返回：0 … 65535。`dev == NULL` 时返回 0。

AD5662 不可回读，这里返回的是驱动自己记录的值，不是从芯片读回来的。SPI 传输失败时不会更新。

---

### 4.8 `AD5662_WriteFrame`（内部）

```c
static HAL_StatusTypeDef AD5662_WriteFrame(AD5662_t *dev, uint16_t code,
                                           AD5662_PowerDown_t mode);
```

组一帧 24 位数据并经 SPI 发出，上面除换算类函数外全部经由它。不对外暴露。

---

## 5. 协议细节

### 24 位帧格式（MSB 先行）

| 位 | 含义 |
|---|---|
| DB23 … DB18 | 无关位 |
| DB17 | PD1 |
| DB16 | PD0 |
| DB15 … DB0 | 16 位 DAC 数据 |

拆成 3 字节发送：

```c
buf[0] = mode & 0x03;      // DB23..DB16
buf[1] = code >> 8;        // DB15..DB8
buf[2] = code & 0x00FF;    // DB7 ..DB0
```

第 24 个 SCLK **下降沿**把整帧写入 DAC 寄存器，SYNC **上升沿**锁存。

### SYNC 必须包住整帧

SYNC 由 GPIO 手动控制，**不用硬件 NSS**。硬件 NSS 会每字节拉高一次，而 AD5662 把 SYNC 提前拉高视为**中断帧**——移位寄存器复位，整帧丢弃，DAC 寄存器不更新。

### SCLK 中途停顿不影响

帧边界由 SYNC 界定，不要求时钟连续。即使中断打断了传输，只要 SYNC 保持低，移位寄存器会继续等待后续时钟。

### 传输失败也会拉高 SYNC

`AD5662_WriteFrame` 里无论 `HAL_SPI_Transmit` 成败都会把 SYNC 拉高。如果失败时把它留在低电平，器件会一直等待后续时钟，下一帧的前几个边沿就会被错当成上一帧的续传。

### 写函数不可重入

`AD5662_WriteFrame` 的 **"SYNC 拉低 → SPI 传输 → SYNC 拉高" 不是原子序列**。如果在中断和主循环里同时调用写函数，两帧会交错，SYNC 被提前拉高，器件按上面那条规则把整帧丢弃——现象是"偶尔一次写入不生效"，很难查。

三个写函数（`AD5662_WriteCode` / `AD5662_SetVoltage` / `AD5662_SetPowerDown`）必须在单一上下文里调用，或者自行加临界区。

`AD5662_VoltageToCode` / `AD5662_CodeToVoltage` 不碰总线，可以在中断里安全调用。

### 掉电模式参数会被校验

`AD5662_PowerDown_t` 只有 4 个合法值。传进非法值时 `AD5662_WriteFrame` 直接返回 `HAL_ERROR` 且**不动 SYNC**（总线上无动作），不会像 `mode & 0x03` 那样被静默截断成某个合法模式。

### 传递函数

```
VOUT = VREF × D / 65536        D = 0 … 65535
```

增益为 1，输出轨到轨。VREF = 3.3V 时 1 LSB = 3.3 / 65536 ≈ **50.35 µV**，满量程对应 VREF 本身。

---

## 6. 用法示例

```c
#include "AD5662.h"

static AD5662_t dac;

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_SPI2_Init();                                    /* 需先在 CubeMX 里启用 SPI2 */

  AD5662_Init(&dac, &hspi2, GPIOB, GPIO_PIN_12, 3.3f);

  AD5662_SetVoltage(&dac, 1.5f);                     /* 输出 1.5V */
  HAL_Delay(100);
  AD5662_SetVoltage(&dac, 3.3f);                     /* 满量程 */
  AD5662_SetVoltage(&dac, 20.0f);                    /* 超量程，钳到 3.3V */

  AD5662_WriteCode(&dac, 0x8000);                    /* 直接给码值，输出半个 VREF = 1.65V */

  AD5662_SetPowerDown(&dac, AD5662_PD_TRISTATE);     /* 进三态 */
  AD5662_SetVoltage(&dac, 1.0f);                     /* 写新码值自动唤醒 */

  while (1) { }
}
```

常见电压对照（vref = 3.3V）：

| 目标电压 | 码值 | 十六进制 |
|---|---|---|
| 0 V | 0 | `0x0000` |
| 0.5 V | 9930 | `0x26CA` |
| 1.0 V | 19859 | `0x4D93` |
| 1.65 V | 32768 | `0x8000` |
| 2.0 V | 39719 | `0x9B27` |
| 3.3 V | 65535 | `0xFFFF` |

---

## 7. 注意事项

- **器件只写**，`AD5662_GetCode()` 返回的是软件记录值，不是芯片回读值。
- **输出无法达到 VREF 的轨**：满量程码 65535 对应 `3.3 × 65535/65536 ≈ 3.29995 V`，比 VREF 低 1 LSB。且输出放大器在接近轨时会有几十 mV 的压降，实际满量程输出略低于 3.3V，需要精确满量程时应实测标定。
- **VREF 直接取自 3.3V 电源时，输出精度受电源纹波影响**。要求高的话应外接基准源（如 ADR444），驱动代码无需改动，只改 `AD5662_Init()` 的 `vref` 实参。
- `AD5662_VoltageToCode()` / `AD5662_CodeToVoltage()` 不发起传输，可以在中断或高频循环里安全调用；其余函数会阻塞在 SPI 传输上（9 MHz 下 3 字节约 2.7 µs）。写函数**不可重入**，见第 5 节。
- **忘记调 `AD5662_Init()` 不会 HardFault**。`AD5662_WriteFrame()` 在动手之前会校验句柄：`dev` / `dev->hspi` / `dev->sync_port` 任一为 `NULL` 就返回 `HAL_ERROR`，总线上无动作。这一条不是多余的——`static AD5662_t dac;` 是零初始化的，忘了 Init 直接调 `AD5662_SetVoltage()` 时这三个指针全是 `NULL`，而 `HAL_GPIO_WritePin()` **完全不校验 `GPIOx`**、`HAL_SPI_Transmit()` 也只校验 `pData`，两者都会直接解引用空指针。
- **`AD5662_Init()` 同样校验 `dev` / `hspi` / `sync_port`**，任一为 `NULL` 就直接返回，不做任何事。
- **`AD5662_VoltageToCode()` 会挡住 `NaN`**：`NaN` 与任何值比较都为假，不挡住的话会一路穿到 `(uint16_t)` 强转，而浮点转无符号整数在值不可表示时是未定义行为（C11 6.3.1.4）。`dev->vref` 和 `voltage` **两个**入参都做了这道防护，判断都写成 `!(x > 0.0f)` 而不是 `x <= 0.0f`——后者挡不住 `NaN`。`AD5662_SetVoltage()` 传入 `NaN` 时会退化成输出 0V。
