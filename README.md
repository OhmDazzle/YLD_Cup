# YLD_Cup

优利德杯 A 题 —— STM32F103C8T6（LQFP48）工程，CLion + CMake + STM32CubeMX（HAL 库）。

## 当前实现

- 上电：OLED 初始化并显示 V_OUT / V_IN / I_IN / I_OUT 四行标签（6×8 字体），AD5662 初始化后输出 1.23V
- 主循环（100ms 一轮）：
  - 采集四路 12 位 ADC（每通道 4 次过采样求均值）：PA6 / PA7 测 V_IN / V_OUT（经分压网络），PB0 / PB1 测 I_OUT / I_IN（分流电阻）
  - 换算后在 OLED 上局部刷新四行数值，不重画标签、不整屏刷新

## 硬件平台

| 项 | 值 |
| --- | --- |
| MCU | STM32F103C8T6（64KB Flash / 20KB RAM） |
| 系统时钟 | HSE 8MHz 晶振 × PLL9 = 72MHz（APB1 36MHz / APB2 72MHz） |
| ADC 时钟 | PCLK2 ÷ 6 = 12MHz（ADC1 / ADC2 独立使用） |
| 调试接口 | SWD（PA13 / PA14） |
| 工具链 | STM32CubeMX 6.18.1（FW_F1 V1.8.7）、CLion + CMake ≥ 3.22、arm-none-eabi-gcc、Ninja |

## 引脚分配

| 引脚 | 配置 | 用途 |
| --- | --- | --- |
| PA4 | GPIO 开漏输出 | OLED SCL（软件 I2C，驱动运行时自行配置） |
| PA5 | GPIO 开漏输出 | OLED SDA（软件 I2C） |
| PA6 | ADC1_IN6 | V_IN 采样（经分压网络） |
| PA7 | ADC1_IN7 | V_OUT 采样（经分压网络） |
| PB0 | ADC2_IN8 | I_OUT 采样（经 R7 分流） |
| PB1 | ADC2_IN9 | I_IN 采样（经 R6 分流） |
| PB12 | GPIO 输出 | AD5662 SYNC（低有效片选） |
| PB13 | SPI2_SCK | AD5662 SCLK |
| PB15 | SPI2_MOSI | AD5662 DIN（PB14 未使用） |
| PA2 | GPIO 输出 | 三极管控制（预留，未接逻辑） |
| PB10 | GPIO 输出 | 三极管控制（预留，未接逻辑） |
| PA8 / PB9 / PB11 | GPIO 输入上拉 | 按键（预留，未接逻辑） |
| PA13 / PA14 | SWD | 调试下载 |
| PD0 / PD1 | HSE | 8MHz 晶振 |

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `Core/` | 业务主逻辑（`Core/Src/main.c`）与 HAL 初始化；CubeMX 生成区不要手改 |
| `Drivers/` | CMSIS 与 STM32F1xx HAL（CubeMX 生成） |
| `AD5662/` | 16 位 DAC 驱动（SPI2），用法见 [AD5662/README.md](AD5662/README.md) |
| `OLED/` | 0.96 寸 OLED 驱动（江协科技 V2.0，软件 I2C），用法见 [OLED/README.md](OLED/README.md) |
| `cmake/` | arm-none-eabi-gcc 工具链文件与 CubeMX 源文件清单 |
| `YLD_Cup.ioc` | CubeMX 工程文件（与代码存在差异，见"注意事项"） |

## 可调参数

集中在 `Core/Src/main.c` 顶部：

| 名称 | 当前值 | 说明 |
| --- | --- | --- |
| `ADC_SAMPLING_TIME` | 55.5 周期 | ADC 采样时间，信号源内阻大时可调大 |
| `ADC_OVERSAMPLING` | 4 | 每通道过采样次数（均值滤波） |
| `V_DIVIDER_RATIO` | 7.0 | V_IN / V_OUT 分压比，待实测确认 |
| `R_6` / `R_7` | 0.005 Ω | I_IN / I_OUT 分流电阻，以实物为准 |
| 主循环周期 | 100 ms | `HAL_Delay` 控制刷新节奏 |

## 构建

需要 arm-none-eabi-gcc 与 Ninja（ST 的 CubeCLT 已自带）：

```bash
cmake --preset Debug          # 配置，输出到 build/Debug
cmake --build --preset Debug  # 编译
```

CLion 中直接使用 Debug / Release 预设构建、烧录即可（SWD）。

## 注意事项

1. **`.ioc` 与代码不完全同步**：SPI2 的 CPOL / CPHA / DataSize / FirstBit / NSS / 分频没有全部存进 `.ioc`，重新生成代码前先看 [AD5662/README.md](AD5662/README.md) 第 2 节，避免 DAC 输出整体错位。
2. **SPI2 分频两个口径**：AD5662 文档推荐 ÷4（9MHz，保裕量），当前代码是 ÷2（18MHz），都在 AD5662 的 20MHz 上限内；下次重新生成前统一成一个口径。
3. **PB12（SYNC）上电为低**：到 `AD5662_Init()` 才拉高，目前无害（期间无 SCLK），后续可在 CubeMX 里把初始电平改成 High。
4. **电流换算待确认**：`I_IN = INA_I_OUT * 4`、`I_OUT = INA_I_OUT * 4`，I_IN 目前取自 I_OUT 的采样值，×4 系数也没有注释，需要对照硬件确认。
5. **OLED 局部刷新以页为粒度**（8px）：各行 y 保持在页边界（0 / 16 / 32 / 48）；改布局时注意 `OLED_UpdateArea` 会连带刷新同一页的其他内容。

## 待办

- [ ] 分压电阻定值后更新 `V_DIVIDER_RATIO`
- [ ] 确认电流换算系数，修正 I_IN 取值
- [ ] 接入按键与三极管控制逻辑
- [ ] 按题目要求补齐其余功能
