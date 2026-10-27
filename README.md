# YLD_Cup

优利德杯 A 题 —— STM32G431CBT6（LQFP48）工程，CLion + CMake + STM32CubeMX（G4 HAL）。

当前版本 **V2.08 电压实测校准**：按《实验数据V2.pdf》第二页先分负载拟合输出，再拟合系数随电流的关系，CV 按最新电流更新 DAC 前馈；按第一页修正 V2.06/V2.07 的 ADC 输出电压残差。主程序和独立恒流调参固件共用测量校准；保留原电流系数及无源蜂鸣器 PWM 驱动。公式、范围与回代结果见 [电压校准说明](calibration/voltage_v2_2026-10-27.md)，历史基础校准见 [实测换算说明](calibration/README.md)。

主程序及各测试文件的启动、循环、按键和故障处理流程，见 [主程序与测试流程 README](README_程序流程.md)。

恒流系数的实测调参使用独立目标 **`YLD_Cup_CCPIDTune`**：自动识别 PI/PID 初值、OLED 查看结果、恒流阶跃验证和手动试参，详见 [恒流调参说明](TEST_PID/README.md)。该固件独立烧录，主程序恒流占位逻辑仍需后续接入。

## 当前实现

- 上电：输出 MOS 保持关断（负端断开，正端仍可能带电），蜂鸣器响 3 声（间隔 0.5s），屏幕显示 "Power Manager" + 状态
- 主逻辑框架（`Core/Src/main.c` 用户区，运行参数在文件头部，校准系数在共享头文件）：
  - 状态机：待机 / 恒压 / 恒流 / 过流保护 / 欠压保护 / ADC 故障；`App_ControlStep()` 执行 CV 电流前馈，电压 PID 和恒流 PID 尚未实现
  - 采样调度：按"电压组 : 电流组"的时隙占比轮流采集——恒压偏电压、恒流偏电流，比例在文件顶部 `CV_VOLT_SLOTS` / `CC_CURR_SLOTS` 等宏自定义
  - 按键：K1=B15 升、K2=B14 降、K3=B13 功能键（待机↔恒压、长按切恒流/恒压、过流解除、欠压静音）
  - 保护：Iout>2.3A 持续 50ms 确认后关输出+报警（手动解除）；运行中 Vin<12.5V 连续确认 3 秒后关输出+报警（正常电压样本清零计时，恢复到13V后自动回原模式）；合闸前已欠压仍立即禁止输出并报警
- 测量链路：四路 12 位 ADC（通道切换后丢弃首样本，再 4 次过采样求均值；640.5 周期采样）——PA7 / PA6 测 Vin / Vout（÷7 分压），PB1 / PB0 测 Iin / Iout（20mΩ 采样电阻 + INA199A1 ×50）；屏幕数值再取最近 3 次刷新（约 300ms）的滑动平均
- DAC：G431 内部 12 位 DAC1_OUT1（PA4）→ OPA320 缓冲 → R13=2.2k 注入 LM2596 FB；先按实测两步模型反解原设定量，再通过采集时的基础关系换算 DAC，方向仍为 DAC↑ → 输出↓

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
| PB0 | ADC1_IN15 | I_OUT 采样（经 R1 20mΩ 分流 + INA199A1） |
| PB1 | ADC1_IN12 | I_IN 采样（经 R6 20mΩ 分流 + INA199A1） |
| PA1 | GPIO 输出 | 输出通断 MOS 控制（高电平=导通） |
| PA2 | TIM2_CH3 / AF1 | 无源蜂鸣器 Q2 控制：约 2.4kHz PWM 发声，低电平静音 |
| PB3 | GPIO 开漏 | OLED SCL（软件 I2C，驱动运行时自行配置） |
| PB4 | GPIO 开漏 | OLED SDA（软件 I2C） |
| PB15 / PB13 / PB14 | GPIO 输入上拉 | 按键 K1(升) / K3(功能) / K2(降)，按下=低 |
| PA13 / PA14 | SWD | 调试下载 |
| PF0 / PF1 | HSE | 8MHz 晶振 |
| PB12 | 未使用 | 外部 DAC 已取消；PB13/PB15 改作按键 |

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `Core/` | 业务主逻辑（`Core/Src/main.c`，用法见 [Core/README.md](Core/README.md)）与 HAL 初始化；CubeMX 生成区不要手改 |
| `Core/Src/buzzer.c`、`Core/Inc/buzzer.h` | 共用无源蜂鸣器驱动，独占 TIM2；不使用中断或 DMA |
| `Drivers/` | CMSIS 与 STM32G4xx HAL（CubeMX 生成） |
| `OLED/` | 0.96 寸 OLED 驱动（江协科技 V2.0，软件 I2C），用法见 [OLED/README.md](OLED/README.md) |
| `TEST/` | 独立采样标定固件（单独编译/烧录，实测采样电阻与分压系数），用法见 [TEST/README.md](TEST/README.md) |
| `TEST_OLED/` | 独立 OLED 测试固件（单独编译/烧录，六页图形/文字/动画自检，K3 长按控制蜂鸣器），用法见 [TEST_OLED/README.md](TEST_OLED/README.md) |
| `TEST_MODULES/` | 独立 DAC/运放、四路 ADC、按键/蜂鸣器测试，各自单独烧录，步骤见 [模块测试说明](TEST_MODULES/README.md) |
| `cmake/` | arm-none-eabi-gcc 工具链文件与 CubeMX 源文件清单 |
| `YLD_CUP.ioc` | CubeMX 工程文件（G431 配置，重新生成前先看"注意事项"） |

## 可调参数

运行参数集中在 `Core/Src/main.c` 顶部；实测换算参数位于 `Core/Inc/measurement_calibration.h` 和 `Core/Inc/voltage_calibration.h`：

| 名称 | 当前值 | 说明 |
| --- | --- | --- |
| `ADC_SAMPLING_TIME` | 640.5 周期 | ADC 采样时间（60kΩ/10kΩ 分压，保留最长档） |
| `ADC_OVERSAMPLING` | 4 | 每通道均值次数（通道切换后的首个样本另外丢弃） |
| `CV_VOLT_SLOTS` : `CV_CURR_SLOTS` | 4 : 1 | 恒压模式"电压:电流"采样时隙比 |
| `CC_VOLT_SLOTS` : `CC_CURR_SLOTS` | 1 : 4 | 恒流模式"电压:电流"采样时隙比 |
| 测量校准 | `Core/Inc/measurement_calibration.h` | 分压名义比 7，电流名义值 1A/V，再按实测二次校正；Vout 增加电流补偿，详见 calibration/README.md |
| DAC 校准 | `Core/Inc/voltage_calibration.h` | 两步实测模型反解，CV 使用最新电流；边界处理见电压校准说明 |
| `OCP_TRIP_A` / `OCP_CONFIRM_MS` | 2.3 A / 50 ms | 过流阈值与确认窗口 |
| `UVP_TRIP_V` / `UVP_CONFIRM_MS` / `UVP_RECOVER_V` | 12.5 V / 3000 ms / 13.0 V | 运行中欠压阈值、连续确认窗口与恢复阈值（V_IN 在 D4 后）；合闸前已欠压仍立即禁止输出并报警 |
| 主循环节拍 | 10 ms | `APP_LOOP_PERIOD_MS` |
| 显示平滑窗口 | 3 × 100 ms | `DISPLAY_AVG_TICKS` × `DISPLAY_REFRESH_MS`，仅平滑屏幕数值 |

## 构建

需要 arm-none-eabi-gcc 与 Ninja（ST 的 CubeCLT 已自带）：

```bash
cmake --preset Debug          # 配置，输出到 build/Debug
cmake --build --preset Debug  # 编译主固件及全部独立测试固件

# 只编译主固件
cmake --build --preset Debug --target YLD_Cup

# 只编译独立采样标定固件
cmake --build --preset Debug --target YLD_Cup_Calib

# 只编译独立 OLED 测试固件
cmake --build --preset Debug --target YLD_Cup_OLEDTest

# 分别编译 DAC/运放、ADC、按键/蜂鸣器测试
cmake --build --preset Debug --target YLD_Cup_OpampTest
cmake --build --preset Debug --target YLD_Cup_ADCTest
cmake --build --preset Debug --target YLD_Cup_GPIOTest
```

CLion 中直接使用 Debug / Release 预设构建、烧录即可（SWD）。

模块测试的接线、按键及预期读数见 [TEST_MODULES/README.md](TEST_MODULES/README.md)。逻辑测试命令为 `python3 tests/run_module_logic.py`；实际 DAC/运放和 INA199 链路仍需万用表、已知电压/电流台架验证。

## 重新生成代码（CubeMX）

本工程的自写代码全部放在 CubeMX 标准用户区块内，重新生成不会丢失：

| 位置 | 内容 | 重新生成时 |
| --- | --- | --- |
| `main.c` 的 `USER CODE BEGIN/END` 区块（Includes / PTD / PD / PV / PFP / 2 / 3 / 4 / MX_GPIO_Init_2 / Error_Handler_Debug） | 全部业务逻辑与可调参数 | 原样保留 |
| `OLED/`、`TEST/`、`TEST_OLED/`、`TEST_MODULES/` 目录 | 自建驱动 / 独立测试固件 | CubeMX 不管理，不受影响 |
| `Core/Src/buzzer.c`、`Core/Inc/buzzer.h` | 共用无源蜂鸣器驱动 | 自建文件，CubeMX 不管理 |
| 顶层 `CMakeLists.txt` | 自加源文件与头文件路径 | 只在首次生成，之后不再重写 |
| `main.c` 区块之外（`MX_*_Init()`、`SystemClock_Config()` 等）和 `cmake/stm32cubemx/` | CubeMX 生成区 | **会被覆盖重写** |

所以：不要手改生成区。要改时钟、引脚或 ADC/DAC 参数，请改 `.ioc` 后重新生成，否则下次会被冲掉。重新生成按下面清单走：

1. 先 `git add -A && git commit`，生成后好 diff、好回退。
2. 确认 HSE 8MHz → PLL /2 ×85 /2 = 170MHz，Boost + 4WS。
3. 确认 ADC1：PB0=IN15、PB1=IN12，单端，640.5 周期，ADC 时钟 /8。
4. 确认 ADC2：PA6=IN3、PA7=IN4，单端，640.5 周期，ADC 时钟 /8。
5. 确认 DAC1：PA4=DAC1_OUT1，输出缓冲使能。
6. 确认 GPIO：PA1 输出 MOS、PA2 初始为低电平 GPIO、PB3/PB4 OLED、PB15/PB14/PB13 按键上拉。`Buzzer_Init()` 在 GPIO 初始化后将 PA2 切到 AF1/TIM2_CH3；TIM2 由共用驱动独占管理，无需在 `.ioc` 另开 TIM2。`buzzer.c/.h` 和顶层 CMake 的驱动源文件配置均保留。
7. 生成后编译烧录，验证 DAC 输出、OLED、按键与保护流程。

## 注意事项

1. **R13 按 2.2kΩ**：理论 `Vout = 17.07091 − 4.545455 × Vdac`；DAC 缓冲输出限于 0.2~3.1V，V2.08 使用两步实测校准，8V/2A 时约 1.891V。功率级压差仍可能限制 16V 输入时的 15V 带载输出。
2. **采样电阻按 20mΩ**：INA199A1 增益 50，名义换算为 1.0 A/V。本版系数由该板实测拟合；若换为 5mΩ 或更换放大器，需重新采集并拟合，不能直接沿用实测系数。
3. **VREF+/VDDA 必须是 3.3V**：ADC/DAC 的换算都按 3.3V；若 VR+ 接了其他基准，需同步修改 `ADC_VREF` 与 `DAC_VMAX`。
4. **输出 MOS 是 PA1，按键为 PB15/PB14/PB13**。PB12/PB13/PB15 旁的旧外部 DAC 标签已无相应器件，不使用 SPI；PB13/PB15 用作按键。
5. **OLED 在 PB3/PB4**：PB3/PB4 复位后是 JTAG 引脚，SWD 模式下可作 GPIO；驱动会把它们配成开漏。
6. **测量标定**：基础校准由 `calibration/fit_measurements.py` 生成；新电压头文件由 `calibration/fit_voltage_v2.py` 生成。更换分压、采样电阻、放大器或板卡后需重新采集和拟合，不能沿用本板系数。
7. **待机/保护时 Vout 显示 0.00V 表示输出禁用**：Q3 切断负端，PA6 测的是正端对系统地；显示值不是对端子压差的独立测量，正端仍可能带电。运行时 Q3 的压降也会影响实际端子电压。
8. **电压分压已改为 60kΩ/10kΩ，比例仍为 7**：节点对地 100nF，RC 时间常数约 0.857ms；固件保留 640.5 周期采样及切换后的弃样。
9. **ADC 时钟 21.25MHz**：G431 ADC 上限 60MHz；当前 170/8，留足了高阻采样的建立时间，不要把分频改回 /2。

## 待办

- [ ] 在 CV 前馈基础上实现电压闭环 PID，并实现主程序恒流 PID
- [ ] 用独立台架读数复核 V2.08 电压补偿，补测 9 至 15V、0 至 0.5A、负载瞬态及 2.3A 保护点；Vin 需同节点参考值
- [ ] 台架实测：开机蜂鸣/待机/恒压切换、保护触发与恢复流程
- [ ] 复核 R13=2.2kΩ，验证 8V / 5~15V 输出范围
- [ ] 重画 G431 版的程序逻辑框架图（旧 F103 框图已删除）

## 本版标定与故障处理

- 电流链路采用 20mΩ + INA199A1 ×50，REF 接地；V2.06 在名义 1A/V 后使用实测二次校正，Vout 再做电流相关补偿。拟合样本、排除项、公式及范围见 [实测换算说明](calibration/README.md)。旧 `TEST/` 仍是提供原始读数的独立工具，不应用正式版系数。
- ADC 调用失败立即关输出并锁存 `ADC FAULT!`，K3 重试成功后回待机，再按一次才开输出。真实 0V/0A 与采样失败分开处理。运行中欠压按新电压样本连续确认3秒，正常样本清零；计时不受300ms过流屏蔽期影响。合闸前重新采样，已欠压则立即禁止输出并报警。
- 用户区配置新按键，并同步 `.ioc`；无需修改 CubeMX 生成区即可构建，重新生成后仍保留用户区设置。
- 主机模拟验证：`python3 tests/run_main_logic.py`，覆盖换算、DAC 限幅、ADC 故障、保护与按键，不替代台架测试。
