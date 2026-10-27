# Core 使用须知（main.c 业务逻辑）

本目录的 `Src/main.c` 是工程主逻辑：CubeMX 生成的初始化骨架 + 写在 `USER CODE` 区块内的业务代码。**只改 `USER CODE` 区块**；重新生成的安全规则见根 [README.md](../README.md) 的“重新生成代码（CubeMX）”一节。

一句话版：**改参数看第 2 节，改逻辑看第 4 节，生成区别碰。**

## 1. 代码地图（USER CODE 区块 → 内容）

| 区块 | 内容 |
| --- | --- |
| Includes | `OLED.h`；新增头文件放这里 |
| PTD | `AppState_t`（状态机）、`KeyEvent_t` / `Key_t`（按键） |
| PD | **时序、目标值及保护参数；实测系数见共享头文件**（第 2 节速查） |
| PV | 状态、设定值、测量值、按键对象、循环节拍 |
| PFP | 函数原型；新增函数必须在这里声明 |
| 2 | 上电流程：蜂鸣器 PWM 初始化（静音）→ OLED → 关输出 → DAC 预置 → ADC 校准 → 蜂鸣 3 声 → 待机 |
| 3 | 主循环：按键 → 采样 → 控制 → 保护 → 显示 → 节拍延时（10 ms） |
| 4 | 函数实现：`ADC_ReadVoltage()` + 全部 `App_*()` |

## 2. 可调参数速查（运行参数在 PD 区，实测系数在共享头文件）

无源蜂鸣器使用独立共用驱动 `Src/buzzer.c` / `Inc/buzzer.h`：PA2/AF1/TIM2_CH3，独占 TIM2，无中断或 DMA。当前 170MHz 定时器时钟下 PSC=169、ARR=416，发声 CCR3=208，约 2398Hz；静音 CCR3=0。音调改 `BUZZER_TONE_HZ`，响停节奏仍改 `main.c` 的 `BEEP_*`。`.ioc` 保留 PA2 初始低电平 GPIO，用户区调用 `Buzzer_Init()` 后切换为 PWM，重生成无需另配 TIM2。

### 2.1 采样时间占比（恒压偏电压 / 恒流偏电流）

| 宏 | 当前值 | 含义 |
| --- | --- | --- |
| `CV_VOLT_SLOTS` : `CV_CURR_SLOTS` | 4 : 1 | 恒压模式每 5 拍里 4 拍采电压组（Vin/Vout）、1 拍采电流组（Iin/Iout） |
| `CC_VOLT_SLOTS` : `CC_CURR_SLOTS` | 1 : 4 | 恒流模式反过来（偏向电流） |

改占比：直接改这四个正整数（比如 9 : 1）；两边都配 0 时退化为只采电压组（防呆分支）。

### 2.2 实测换算

V2.06 在 `Core/Inc/measurement_calibration.h` 统一保存系数及 `Cal_Iin()`、`Cal_Iout()`、`Cal_Vout()`，主程序与独立恒流调参固件共用。头文件由 `calibration/fit_measurements.py` 根据 35 组原记录生成；公式、范围和回代残差见 [实测换算说明](../calibration/README.md)。

| 量 | 计算方式 | 测量口径 |
| --- | --- | --- |
| Vin | PA7 引脚电压 × 7 | D4 后节点；参考表在 D4 前，不能据此拟合 ADC 比例 |
| Iin / Iout | 引脚电压按 1A/V 得名义电流，再做独立二次校正 | 超出实测上限按正斜率切线延伸，负值钳到 0A，不做上电归零 |
| Vout | PA6 名义电压 × 系数 + 校正 Iout × 系数 + 截距 | 端子电压估计值；电压组和电流组刷新时都从 `measVoutNominal` 重算 |

`TEST/` 和 `YLD_Cup_ADCTest` 继续提供原始或名义读数，便于以后重新采集；没有叠加正式版校准，避免对同一组读数重复校正。更换板卡或采样器件需要重标定。

### 2.3 其它常用宏

| 宏 | 当前值 | 说明 |
| --- | --- | --- |
| `VOUT_TARGET_DEFAULT / MIN / MAX / VOUT_STEP` | 8 / 5 / 15 / 1.0 | 步进调压默认值与范围（题目 2.2） |
| `IOUT_TARGET_* / IOUT_STEP` | 1 / 0.2 / 2 / 0.1 | 恒流模式设定与步进 |
| `OCP_TRIP_A` / `OCP_CONFIRM_MS` | 2.3 A / 50 ms | 过流阈值与确认窗口 |
| `UVP_TRIP_V` / `UVP_CONFIRM_MS` / `UVP_RECOVER_V` | 12.5 V / 3000 ms / 13.0 V | 运行中欠压连续确认3秒，正常电压样本清零；合闸前已欠压仍立即禁止输出并报警 |
| `PROT_BLANK_MS` | 300 | 合闸后的过流屏蔽期；欠压和 ADC 故障不屏蔽 |
| `APP_LOOP_PERIOD_MS` / `DISPLAY_REFRESH_MS` | 10 / 100 ms | 主循环节拍 / 屏幕刷新周期 |
| `DISPLAY_AVG_TICKS` | 3 | 显示滑动平均窗口（3 × 100 ms = 300 ms，仅影响屏幕，保护用实时值） |
| `KEY_DEBOUNCE_MS` / `KEY_LONG_MS` | 20 / 800 ms | 按键消抖 / 长按判定 |
| `BEEP_ON_MS / BEEP_GAP_MS / BEEP_STARTUP_TIMES` | 200 / 500 / 3 | 开机蜂鸣节奏 |
| `VOUT_DAC_A / VOUT_DAC_B` | 17.07091 / 4.545455 | 目标电压 → DAC 传递关系（对应 R13=2.2k；方向：DAC↑ → Vout↓） |
| `DAC_FULL_SCALE` / `DAC_VMAX` | 4095 / 3.3 | G431 12 位 DAC 满量程与参考电压 |

## 3. 按键操作（与屏幕提示行一一对应）

- **K3 = PB13 功能键**：待机 ↔ 恒压（短按）、恒压 ↔ 恒流（长按）、过流解除、欠压静音；
- **ADC 故障时 K3 短按**：重试采样，成功后仅回待机，再按一次才开输出；
- **K1 = PB15 / K2 = PB14**：只在运行状态有效（CV 调 ±1V，CC 调 ±0.1A），待机与保护状态下无动作；
- 长按触发后松手不会再产生短按事件。

## 4. 常见改动怎么做

- **加一个状态**：`AppState_t` 加枚举 → `App_SetState()` 的 switch 加“进入动作” → `App_HandleKeyEvent()` 加按键响应 → `App_StateText()` 加屏幕文案。
- **改按键分工**：只动 `App_HandleKeyEvent()`。
- **改主循环步骤**：`USER CODE 3` 里的 6 个步骤，按注释增删。
- **实现 PID**：填 `App_ControlStep()`（函数头注释里写好了误差定义与负增益方向）。
- **改屏幕布局**：`App_UpdateDisplay()` + PD 区的 `OLED_*` 布局宏。
- **改 DAC 传递关系**：只改 `VOUT_DAC_A / VOUT_DAC_B`；硬件反馈网络变了才需要动。
- **改 ADC 通道**：电压组在 `App_SampleVoltageGroup()`，电流组在 `App_SampleCurrentGroup()`；同时确认 `.ioc` 里的引脚分配。

## 5. 注意事项

1. `App_ControlStep()` 目前是占位——CV/CC 靠开环预置，不会精确闭环，属预期；接线与标定完成后再填 PID。
2. `ADC_ReadVoltage()` 每次调用都会重配 ADC 通道，丢弃切换后的第一个样本，再做 4 次均值转换（640.5 周期采样，一个通道含弃样和均值至少约 150 µs），不要放进中断；采样节奏交给 `App_SampleScheduler()`。
3. 显示是整屏重绘（100 ms 一次）；以后若要改回局部刷新，注意 `OLED_UpdateArea()` 以页（8px）为粒度。
4. 生成区（`MX_*_Init()`、`SystemClock_Config()`）不要手改；要改参数先改 `.ioc` 再生成（流程见根 README 安全清单）。
5. 屏幕数值用驱动自带的 `OLED_ShowFloatNum`，固定带 `+` 号；在意排版再自行格式化。
6. 非运行状态的 Vout=0.00V 仅表示输出禁用：Q3 断开负端，PA6 采正端对系统地，无法直接测浮空负端的端子压差。运行时读数已做电流相关的端子电压经验补偿；后续闭环应结合独立端子实测复核。
7. 屏幕四个测量值取最近 `DISPLAY_AVG_TICKS` 次刷新（默认 300 ms）的滑动平均，仅影响显示；过流/欠压保护仍使用未平滑的实时测量值。状态切换时平均窗口会重置，避免把上一状态的读数带进新状态。
8. DAC 使用内部 DAC1_OUT1（PA4，12 位）；`App_DacSetVoltage()` 里做了 0.2~3.1V 限幅；非有限值取高 DAC 电压以降低输出，不要绕过它直接写 DAC 数据寄存器。
9. 输出 MOS 是 PA1（高电平导通）；PB15=升、PB14=降、PB13=功能，新 GPIO 设置位于 `MX_GPIO_Init_2` 用户区并同步到 `.ioc`。

## 新增参数与采样故障

V2.06 的电流二次系数与输出电压补偿来自实测数据，范围及限制见校准说明。`ADC_ReadVoltage()` 成功返回 1 并写入电压，失败返回 0；一组通道全部成功后才更新测量值。ADC 故障立即关输出，锁存 `ADC FAULT!`，K3 重试成功仅回待机。合闸前重新采两组，欠压时保持关断。按键按实际经过的毫秒数消抖/判长按，支持系统节拍回绕。
