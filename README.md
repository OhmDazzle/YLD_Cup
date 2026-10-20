# YLD_Cup

优利德杯 A 题 —— STM32G431CBT6（LQFP48）工程，CLion + CMake + STM32CubeMX（G4 HAL）。

## 当前实现

- 上电：输出 MOS 保持关断（无输出），蜂鸣器响 3 声（间隔 0.5s），屏幕显示 "Power Manager" + 状态
- 主逻辑框架（`Core/Src/main.c` 用户区，可调参数全部集中在文件头部）：
  - 状态机：待机 / 恒压 / 恒流 / 过流保护 / 欠压保护；闭环 PID 为占位（`App_ControlStep()`），下一版实现
  - 采样调度：按"电压组 : 电流组"的时隙占比轮流采集——恒压偏电压、恒流偏电流，比例在文件顶部 `CV_VOLT_SLOTS` / `CC_CURR_SLOTS` 等宏自定义
  - 按键：K1=A8 升、K2=B11 降、K3=B9 功能键（待机↔恒压、长按切恒流/恒压、过流解除、欠压静音）
  - 保护：Iout>2.3A 持续 50ms 确认后关输出+报警（手动解除）；Vin<12.5V 关输出+报警（恢复输入后自动回原模式）
- 测量链路：四路 12 位 ADC（通道切换后丢弃首样本，再 4 次过采样求均值；640.5 周期采样）——PA7 / PA6 测 Vin / Vout（÷7 分压），PB1 / PB0 测 Iin / Iout（10mΩ 采样电阻 + INA199A1 ×50）；屏幕数值再取最近 3 次刷新（约 300ms）的滑动平均
- DAC：G431 内部 12 位 DAC1_OUT1（PA4）→ OPA320 缓冲 → R13=2.5k 注入 LM2596 FB；`Vout ≈ 16.4 − 4 × Vdac`

## 硬件平台

| 项 | 值 |
| --- | --- |
| MCU | STM32G431CBT6（128KB Flash / 32KB RAM） |
| 系统时钟 | HSE 8MHz 晶振 → PLL /2 ×85 /2 = **170MHz**（Boost 模式，Flash 4WS） |
| ADC 时钟 | SYSCLK / 8 = **21.25MHz**（ADC1 / ADC2 独立使用） |
| DAC | 内部 DAC1_OUT1，12 位，VREF = 3.3V |
| 调试接口 | SWD（PA13 / PA14） |
| 工具链 | STM32CubeMX 6.18.1（FW_G4 V1.6.3）、CLion + CMake ≥ 3.22、arm-none-eabi-gcc、Ninja |

## 引脚分配

| 引脚 | 配置 | 用途 |
| --- | --- | --- |
| PA4 | DAC1_OUT1 | DAC 输出 → OPA320 → LM2596 FB |
| PA6 | ADC2_IN3 | V_OUT 采样（分压网络，输出电压÷7） |
| PA7 | ADC2_IN4 | V_IN 采样（分压网络，输入电压÷7） |
| PB0 | ADC1_IN15 | I_OUT 采样（经 R7 10mΩ 分流 + INA199A1） |
| PB1 | ADC1_IN12 | I_IN 采样（经 R6 10mΩ 分流 + INA199A1） |
| PA1 | GPIO 输出 | 输出通断 MOS 控制（高电平=导通） |
| PA2 | GPIO 输出 | 蜂鸣器 MOS 控制（高电平=响） |
| PB3 | GPIO 开漏 | OLED SCL（软件 I2C，驱动运行时自行配置） |
| PB4 | GPIO 开漏 | OLED SDA（软件 I2C） |
| PA8 / PB9 / PB11 | GPIO 输入上拉 | 按键 K1(升) / K3(功能) / K2(降)，按下=低 |
| PA13 / PA14 | SWD | 调试下载 |
| PF0 / PF1 | HSE | 8MHz 晶振 |
| PB12 / PB13 / PB15 | 未使用 | 旧外部 DAC 的 SYNC/SCLK/DIN 网络已取消 |

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `Core/` | 业务主逻辑（`Core/Src/main.c`，用法见 [Core/README.md](Core/README.md)）与 HAL 初始化；CubeMX 生成区不要手改 |
| `Drivers/` | CMSIS 与 STM32G4xx HAL（CubeMX 生成） |
| `OLED/` | 0.96 寸 OLED 驱动（江协科技 V2.0，软件 I2C），用法见 [OLED/README.md](OLED/README.md) |
| `TEST/` | 独立采样标定固件（单独编译/烧录，实测采样电阻与分压系数），用法见 [TEST/README.md](TEST/README.md) |
| `TEST_OLED/` | 独立 OLED 测试固件（单独编译/烧录，六页图形/文字/动画自检，K3 长按控制蜂鸣器），用法见 [TEST_OLED/README.md](TEST_OLED/README.md) |
| `cmake/` | arm-none-eabi-gcc 工具链文件与 CubeMX 源文件清单 |
| `YLD_CUP.ioc` | CubeMX 工程文件（G431 配置，重新生成前先看"注意事项"） |

## 可调参数

集中在 `Core/Src/main.c` 顶部：

| 名称 | 当前值 | 说明 |
| --- | --- | --- |
| `ADC_SAMPLING_TIME` | 640.5 周期 | ADC 采样时间（10MΩ 级高阻分压必须用最长档） |
| `ADC_OVERSAMPLING` | 4 | 每通道均值次数（通道切换后的首个样本另外丢弃） |
| `CV_VOLT_SLOTS` : `CV_CURR_SLOTS` | 4 : 1 | 恒压模式"电压:电流"采样时隙比 |
| `CC_VOLT_SLOTS` : `CC_CURR_SLOTS` | 1 : 4 | 恒流模式"电压:电流"采样时隙比 |
| `*_ADC_SCALE` | 电压 7.0；电流自动折算 | 线性标定；电流 scale = 1/(采样电阻 × INA199 增益)，电阻 10mΩ、增益 50 |
| `VOUT_DAC_A` / `VOUT_DAC_B` | 16.4 / 4.0 | 目标电压 → DAC 电压 的传递关系（对应 R13=2.5k，待实测修正） |
| `OCP_TRIP_A` / `OCP_CONFIRM_MS` / `UVP_TRIP_V` | 2.3 A / 50 ms / 12.5 V | 过流阈值与确认窗口 / 欠压阈值 |
| 主循环节拍 | 10 ms | `APP_LOOP_PERIOD_MS` |
| 显示平滑窗口 | 3 × 100 ms | `DISPLAY_AVG_TICKS` × `DISPLAY_REFRESH_MS`，仅平滑屏幕数值 |

## 构建

需要 arm-none-eabi-gcc 与 Ninja（ST 的 CubeCLT 已自带）：

```bash
cmake --preset Debug          # 配置，输出到 build/Debug
cmake --build --preset Debug  # 编译主固件 + 两个测试固件

# 只编译主固件
cmake --build --preset Debug --target YLD_Cup

# 只编译独立采样标定固件
cmake --build --preset Debug --target YLD_Cup_Calib

# 只编译独立 OLED 测试固件
cmake --build --preset Debug --target YLD_Cup_OLEDTest
```

CLion 中直接使用 Debug / Release 预设构建、烧录即可（SWD）。

## 重新生成代码（CubeMX）

本工程的自写代码全部放在 CubeMX 标准用户区块内，重新生成不会丢失：

| 位置 | 内容 | 重新生成时 |
| --- | --- | --- |
| `main.c` 的 `USER CODE BEGIN/END` 区块（Includes / PTD / PD / PV / PFP / 2 / 3 / 4） | 全部业务逻辑与可调参数 | 原样保留 |
| `OLED/`、`TEST/`、`TEST_OLED/` 目录 | 自建驱动 / 独立测试固件 | CubeMX 不管理，不受影响 |
| 顶层 `CMakeLists.txt` | 自加源文件与头文件路径 | 只在首次生成，之后不再重写 |
| `main.c` 区块之外（`MX_*_Init()`、`SystemClock_Config()` 等）和 `cmake/stm32cubemx/` | CubeMX 生成区 | **会被覆盖重写** |

所以：不要手改生成区。要改时钟、引脚或 ADC/DAC 参数，请改 `.ioc` 后重新生成，否则下次会被冲掉。重新生成按下面清单走：

1. 先 `git add -A && git commit`，生成后好 diff、好回退。
2. 确认 HSE 8MHz → PLL /2 ×85 /2 = 170MHz，Boost + 4WS。
3. 确认 ADC1：PB0=IN15、PB1=IN12，单端，640.5 周期，ADC 时钟 /8。
4. 确认 ADC2：PA6=IN3、PA7=IN4，单端，640.5 周期，ADC 时钟 /8。
5. 确认 DAC1：PA4=DAC1_OUT1，输出缓冲使能。
6. 确认 GPIO：PA1 输出 MOS、PA2 蜂鸣器、PB3/PB4 OLED、PA8/PB11/PB9 按键上拉。
7. 生成后编译烧录，验证 DAC 输出、OLED、按键与保护流程。

## 注意事项

1. **R13 必须是 2.5k**：代码按 `Vout = 16.4 − 4 × Vdac` 写；若板上仍是 7.5k，输出范围只有 8.72~13.12V，8V 和 5~15V 都达不到。
2. **采样电阻按 10mΩ**：`IIN/IOUT_SHUNT_OHM = 0.010f`，INA199A1 增益 50，电流 scale = 2.0 A/V。若实物是 5mΩ，改这两个宏即可。
3. **VREF+/VDDA 必须是 3.3V**：ADC/DAC 的换算都按 3.3V；若 VR+ 接了其他基准，需同步修改 `ADC_VREF` 与 `DAC_VMAX`。
4. **输出 MOS 是 PA1**：原理图中 PB9 也误标了 `MOS_G`，但 PB9 同时是 K3，实际以 PA1 为准。
5. **OLED 在 PB3/PB4**：PB3/PB4 复位后是 JTAG 引脚，SWD 模式下可作 GPIO；驱动会把它们配成开漏。
6. **测量标定**：四通道线性标定系数（scale）集中在 `main.c` 头部；电流通道单独暴露采样电阻与 INA199 增益，改这两个宏后 scale 自动折算。
7. **待机/保护时 Vout 显示 0.00V**：Vout 采样点在输出通断 MOS 内侧，待机时变换器仍在工作、采样点会读到储能电压；固件在非运行状态强制显示 0.00V。调试变换器内部电压请用 TEST 标定固件看原始值。
8. **电压通道是高阻分压**：VIN / VOUT 经 10MΩ 级网络进 ADC，采样时间必须用最长档（640.5 周期），且每次切换通道后丢弃第一个样本。
9. **ADC 时钟 21.25MHz**：G431 ADC 上限 60MHz；当前 170/8，留足了高阻采样的建立时间，不要把分频改回 /2。

## 待办

- [ ] 实现闭环 PID（CV / CC，`App_ControlStep()` 占位处）
- [ ] 用台式表标定四个测量通道（scale），确认采样电阻 10mΩ 与 INA199 增益档位
- [ ] 台架实测：开机蜂鸣/待机/恒压切换、保护触发与恢复流程
- [ ] 复核 R13 已改为 2.5k，验证 8V / 5~15V 输出范围
- [ ] 重画 G431 版的程序逻辑框架图（旧 F103 框图已删除）
