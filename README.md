# YLD_Cup

优利德杯 A 题 —— STM32F103C8T6（LQFP48）工程，CLion + CMake + STM32CubeMX（HAL 库）。

## 当前实现

- 上电：输出 MOS 保持关断（无输出），蜂鸣器响 3 声（间隔 0.5s），屏幕显示 "Power Manager" + 状态
- 主逻辑框架（`Core/Src/main.c` 用户区，可调参数全部集中在文件头部）：
  - 状态机：待机 / 恒压 / 恒流 / 过流保护 / 欠压保护；闭环 PID 为占位（`App_ControlStep()`），下一版实现
  - 采样调度：按"电压组 : 电流组"的时隙占比轮流采集——恒压偏电压、恒流偏电流，比例在文件顶部 `CV_VOLT_SLOTS` / `CC_CURR_SLOTS` 等宏自定义
  - 按键：K1=A8 升、K2=B11 降、K3=B9 功能键（待机↔恒压、长按切恒流/恒压、过流解除、欠压静音）
  - 保护：Iout>2.3A 持续 50ms 确认后关输出+报警（手动解除）；Vin<12.5V 关输出+报警（恢复输入后自动回原模式）
- 测量链路：四路 12 位 ADC（各 4 次过采样求均值）——PA7 / PA6 测 Vin / Vout（÷7 分压），PB1 / PB0 测 Iin / Iout（采样电阻分流 + INA199，电阻值待实测）

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
| PA6 | ADC1_IN6 | V_OUT 采样（分压网络，输出电压÷7） |
| PA7 | ADC1_IN7 | V_IN 采样（分压网络，输入电压÷7） |
| PB0 | ADC2_IN8 | I_OUT 采样（经 R7 分流） |
| PB1 | ADC2_IN9 | I_IN 采样（经 R6 分流） |
| PB12 | GPIO 输出 | AD5662 SYNC（低有效片选） |
| PB13 | SPI2_SCK | AD5662 SCLK |
| PB15 | SPI2_MOSI | AD5662 DIN（PB14 未使用） |
| PA2 | GPIO 输出 | 蜂鸣器 MOS 控制（高电平=响） |
| PB10 | GPIO 输出 | 输出通断 MOS 控制（高电平=导通） |
| PA8 / PB9 / PB11 | GPIO 输入上拉 | 按键 K1(升) / K3(功能) / K2(降)，按下=低 |
| PA13 / PA14 | SWD | 调试下载 |
| PD0 / PD1 | HSE | 8MHz 晶振 |

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `Core/` | 业务主逻辑（`Core/Src/main.c`，用法见 [Core/README.md](Core/README.md)）与 HAL 初始化；CubeMX 生成区不要手改 |
| `Drivers/` | CMSIS 与 STM32F1xx HAL（CubeMX 生成） |
| `AD5662/` | 16 位 DAC 驱动（SPI2），用法见 [AD5662/README.md](AD5662/README.md) |
| `OLED/` | 0.96 寸 OLED 驱动（江协科技 V2.0，软件 I2C），用法见 [OLED/README.md](OLED/README.md) |
| `TEST/` | 独立采样标定固件（单独编译/烧录，实测采样电阻与分压系数），用法见 [TEST/README.md](TEST/README.md) |
| `TEST_OLED/` | 独立 OLED 测试固件（单独编译/烧录，六页图形/文字/动画自检），用法见 [TEST_OLED/README.md](TEST_OLED/README.md) |
| `cmake/` | arm-none-eabi-gcc 工具链文件与 CubeMX 源文件清单 |
| `YLD_Cup.ioc` | CubeMX 工程文件（与代码存在差异，见"注意事项"） |

## 可调参数

集中在 `Core/Src/main.c` 顶部：

| 名称 | 当前值 | 说明 |
| --- | --- | --- |
| `ADC_SAMPLING_TIME` | 55.5 周期 | ADC 采样时间，信号源内阻大时可调大 |
| `ADC_OVERSAMPLING` | 4 | 每通道过采样次数（均值滤波） |
| `CV_VOLT_SLOTS` : `CV_CURR_SLOTS` | 4 : 1 | 恒压模式"电压:电流"采样时隙比 |
| `CC_VOLT_SLOTS` : `CC_CURR_SLOTS` | 1 : 4 | 恒流模式"电压:电流"采样时隙比 |
| `*_ADC_SCALE` | 电压 7.0；电流自动折算 | 线性标定；电流 scale = 1/(采样电阻 × INA199 增益)，电阻/增益按实物填 `IIN/IOUT_SHUNT_OHM`、`INA199_GAIN` |
| `VOUT_DAC_A` / `VOUT_DAC_B` | 16.4 / 4.0 | 目标电压 → DAC 电压 的传递关系（待实测修正） |
| `OCP_TRIP_A` / `OCP_CONFIRM_MS` / `UVP_TRIP_V` | 2.3 A / 50 ms / 12.5 V | 过流阈值与确认窗口 / 欠压阈值 |
| 主循环节拍 | 10 ms | `APP_LOOP_PERIOD_MS` |

## 构建

需要 arm-none-eabi-gcc 与 Ninja（ST 的 CubeCLT 已自带）：

```bash
cmake --preset Debug          # 配置，输出到 build/Debug
cmake --build --preset Debug  # 编译

# 只编译独立 OLED 测试固件
cmake --build --preset Debug --target YLD_Cup_OLEDTest
```

CLion 中直接使用 Debug / Release 预设构建、烧录即可（SWD）。

## 重新生成代码（CubeMX）

本工程的自写代码全部放在 CubeMX 标准用户区块内，重新生成不会丢失：

| 位置 | 内容 | 重新生成时 |
| --- | --- | --- |
| `main.c` 的 `USER CODE BEGIN/END` 区块（Includes / PTD / PD / PV / PFP / 2 / 3 / 4） | 全部业务逻辑与可调参数 | 原样保留 |
| `AD5662/`、`OLED/`、`TEST/` 目录 | 自建驱动 / 独立标定固件 | CubeMX 不管理，不受影响 |
| 顶层 `CMakeLists.txt` | 自加源文件与头文件路径 | 只在首次生成，之后不再重写 |
| `main.c` 区块之外（`MX_*_Init()`、`SystemClock_Config()` 等）和 `cmake/stm32cubemx/` | CubeMX 生成区 | **会被覆盖重写** |

所以：不要手改生成区（例如 `MX_SPI2_Init()` 里的 CPOL/CPHA）——要改参数请改 `.ioc` 后重新生成，否则下次会被冲掉。重新生成按下面清单走：

1. 先 `git add -A && git commit`，生成后好 diff、好回退。
2. 按 [AD5662/README.md](AD5662/README.md) 第 2 节把 SPI2 参数在 CubeMX 里重配齐（尤其 CPOL=Low + CPHA=2 Edge），并统一分频口径（÷4 9MHz 或 ÷2 18MHz）。
3. 顺手把 PB12 初始电平改成 High（见"注意事项"3）。
4. 生成后重点 diff：`MX_SPI2_Init()` 全部参数、`MX_GPIO_Init()` 的 PA2/PB10/PB12 与按键引脚、外设初始化顺序。
5. 编译烧录，验证 DAC 输出、OLED、按键与保护流程。

## 注意事项

1. **`.ioc` 与代码不完全同步**：SPI2 的 CPOL / CPHA / DataSize / FirstBit / NSS / 分频没有全部存进 `.ioc`，重新生成代码前先看 [AD5662/README.md](AD5662/README.md) 第 2 节，避免 DAC 输出整体错位。
2. **SPI2 分频两个口径**：AD5662 文档推荐 ÷4（9MHz，保裕量），当前代码是 ÷2（18MHz），都在 AD5662 的 20MHz 上限内；下次重新生成前统一成一个口径。
3. **PB12（SYNC）上电为低**：到 `AD5662_Init()` 才拉高，目前无害（期间无 SCLK），后续可在 CubeMX 里把初始电平改成 High。
4. **测量标定**：四通道线性标定系数（offset/scale）集中在 `main.c` 头部；电流通道单独暴露采样电阻（`IIN/IOUT_SHUNT_OHM`，实物待测）与 INA199 增益（`INA199_GAIN`，丝印 OBG=50 / OBH=100 / OBI=200），改这两个宏后 scale 自动折算。
5. **OLED 布局**：6×8 字体、行距 8px（y=0/8/…/56），当前每 100ms 整屏重绘；若后面改回局部刷新，注意 `OLED_UpdateArea` 以页（8px）为粒度。

## 待办

- [ ] 实现闭环 PID（`App_ControlStep()` 占位处）
- [ ] 用台式表标定四个测量通道（offset/scale），并确认采样电阻阻值与 INA199 增益档位——可直接用 [TEST/README.md](TEST/README.md) 的标定模式
- [ ] SPI2 相位改为 2 Edge（CPHA=1，依据 AD5662 手册，见 AD5662/README.md）
- [ ] 台架实测：开机蜂鸣/待机/恒压切换、保护触发与恢复流程
