# 独立模块测试

按 `SCH_Schematic1_3_2026-10-20.pdf` 编写。每个目标单独编译、烧录，有自己的 `main()`；不运行 `Core/Src/main.c`。新增测试共用 `test_board.c` 的时钟、GPIO 和采样初始化，便于保持引脚一致。

## 测试入口

建议按表格顺序检查。每次仅烧录一份固件，测完重新烧录 `YLD_Cup` 才会恢复业务程序。

| 模块 | 独立源文件 | CMake Target | 检查内容 |
| --- | --- | --- | --- |
| 屏幕 | [oled_test_main.c](../TEST_OLED/oled_test_main.c)（已有） | `YLD_Cup_OLEDTest` | 六页文字、点阵、条纹、图形、动画，见 [屏幕说明](../TEST_OLED/README.md) |
| 按键、蜂鸣器 | [gpio_test_main.c](gpio_test_main.c) | `YLD_Cup_GPIOTest` | 按键状态、次数，分别发出 1/2/3 声 |
| 内部 DAC、OPA320 | [opamp_test_main.c](opamp_test_main.c) | `YLD_Cup_OpampTest` | 六档电压，万用表逐级定位 DAC、运放接线问题 |
| 电压采样、INA199 输出采样 | [adc_test_main.c](adc_test_main.c) | `YLD_Cup_ADCTest` | 四路原始码、引脚电压、未标定的 V/A 换算 |
| 降压功率级、输出 MOS、带载电流标定 | [calib_main.c](../TEST/calib_main.c)（已有） | `YLD_Cup_Calib` | 开环目标电压、手动通断、四路采样，见 [标定说明](../TEST/README.md) |

```sh
cmake --preset Debug
cmake --build --preset Debug --target YLD_Cup_GPIOTest
cmake --build --preset Debug --target YLD_Cup_OpampTest
cmake --build --preset Debug --target YLD_Cup_ADCTest
```

新增 ELF 在 `build/Debug/TEST_MODULES/<Target>.elf`；屏幕 ELF 在 `build/Debug/TEST_OLED/`，标定 ELF 在 `build/Debug/TEST/`。CLion 中复制运行/调试配置，选择对应 Target，通过 SWD 下载；不要把测试源文件加入业务目标，也不要同时链接多个 `main()`。

## 先接逻辑电源

屏幕、按键和 DAC/运放测试：**只给逻辑部分接 5V，断开 U10 的 13～20V 主输入及输出负载**。检查 3.3V、VDDA/VREF+ 都正常，再测信号，万用表黑笔接系统 GND。

三个新增固件都把 PA1 保持低电平，Q3 始终关断。Q3 切断输出负端，**不切断 LM2596 及正端供电**；带主输入扫 DAC 仍会改变正端对地电压，不能把“Q3 OFF”当成正端断电。外接仪器地线也不能跨接 Q3 负端回路。

## 按键、蜂鸣器

使用 `YLD_Cup_GPIOTest`：

| 按键 | MCU 引脚 | 预期表现 |
| --- | --- | --- |
| K1 | PB15 | 显示 DOWN，计数加 1，蜂鸣器响 1 声 |
| K2 | PB14 | 显示 DOWN，计数加 1，蜂鸣器响 2 声 |
| K3 | PB13 | 显示 DOWN，计数加 1，蜂鸣器响 3 声 |

松手显示 UP；按住不连发。20ms 消抖，新按键覆盖尚未完成的蜂鸣序列。每声高电平 100ms、间隔 100ms；PA2 驱动 Q2，按有源蜂鸣器使用。若实物为无源蜂鸣器，这种门控只可能产生点击声，需要载波驱动，不能仅凭“不响”判断 Q2 损坏。

没屏幕也能听声，并在调试器看 `gpio_press_count[3]`、`gpio_last_key`、`tb_status.keys_raw/keys_down`。若计数正确但不响，测 PA2 是否出现高脉冲，再查 Q2 栅极、蜂鸣器供电及类型。三个键松开应为高、按下应接近 0V。

## DAC 与运放

使用 `YLD_Cup_OpampTest`，默认固定 2.0V。K1 升一档，K2 降一档，到边界停止；K3 切换自动模式，每 5 秒循环一档。手动切档退出自动模式。

| 设定电压 | DAC 码值（VREF=3.3V） | PA4、U2 的 3/1/4 脚预期 |
| --- | --- | --- |
| 0.5V | 620 | 均接近 0.5V |
| 1.0V | 1241 | 均接近 1.0V |
| 1.5V | 1861 | 均接近 1.5V |
| 2.0V（默认） | 2482 | 均接近 2.0V |
| 2.5V | 3102 | 均接近 2.5V |
| 3.0V | 3723 | 均接近 3.0V |

屏幕 `CMD` 是按码值和假定 3.3V 基准计算的命令电压，**没有测量运放输出，也不会自动判断 PASS**。实际基准偏差会按比例影响 DAC；先测实际 VREF+，可用 `码值 × 实测VREF / 4095` 核对。

逐点测量（OPA320AIDBVR，U2）：

1. 2 脚对系统地约 0V，5 脚约 3.3V。不要把 5 脚接地。
2. PA4 应随档位变化；不变先排查 DAC 引脚、烧录目标和 VREF+。
3. 3 脚是同相输入，经 R12=1kΩ 接 PA4。等待稳定后应接近 PA4；若 PA4 正常而 3 脚异常，查 R12、C13 和焊接。
4. 1 脚是输出，4 脚是反相输入，两脚直接反馈，应接近 3 脚。输入正确、输出贴近电源轨或不跟随时，查供电、引脚方向、反馈及输出负载/短路。
5. 按六档记录 `PA4 / U2-3 / U2-1 / U2-4`。若万用表平均读数正常但输出不稳定，再用示波器检查振荡；本测试不是运放动态性能或稳定性认证。

没屏幕时默认仍为 2.0V，按键可切档，Watch 中查看 `opamp_level`（0～5）、`opamp_auto`、`tb_status.dac_code/dac_v`。

## 四路 ADC 与采样链路

使用 `YLD_Cup_ADCTest`：固定 DAC 2.0V、Q3 关闭，默认显示四路原始码和 ADC 引脚电压；每通道切换后弃首样，再平均 16 次，每 100ms 采一组。

| 行 | 引脚 / ADC 通道 | 原理图换算（未标定） |
| --- | --- | --- |
| VIN | PA7 / ADC2_IN4 | 引脚电压 ×7，60kΩ/10kΩ 分压 |
| VOUT | PA6 / ADC2_IN3 | 引脚电压 ×7，正端对系统地电压 |
| IIN | PB1 / ADC1_IN12 | 引脚电压 ÷(0.020Ω ×50)，即 1A/V |
| IOUT | PB0 / ADC1_IN15 | 引脚电压 ÷(0.020Ω ×50)，即 1A/V |

K1 切换引脚 V / 物理 V、A，K2 清累计采样错误次数，K3 暂停/继续；`HOLD` 表示旧读数。换算按 INA199A1、REF 接地，零点和增益未标定；零电流可能读到小偏移，不自动清零。

先只接逻辑 5V，用万用表测 PA7/PA6/PB1/PB0 对地电压，与屏幕 **PIN VOLTS** 比较。约 1.000V 对应码值 1241，2.000V 对应 2482（基准 3.3V）。若要给 ADC 引脚注入已知电压，必须先断开该通道原有分压/INA 输出连接，避免反向驱动；输入不能超出 0～实际 VDDA。

之后可接主电源、保持 Q3 关闭，比较 VIN 与电源表，比较 VOUT 与正端对系统地的实测值。DAC 2.0V 对应功率级理想设定约 7.98V，实际由电阻误差、输入余量及电路状态决定。Q3 关闭时 VOUT 对地仍可能非零，不能用它代替两端子间压差。

本固件不合闸，不能用它验证带载 IOUT、Q3 导通或完整 INA 增益。带载测试转用已有标定固件：先断开主电源烧录，接限流电源与小负载，确认开环电压后手动合闸，再对照台式表做两点标定。**标定固件没有业务固件的 OCP/UVP 保护**，测试电流和供电限流由台架控制；不能当闭环电源使用。分流电阻压差应用独立取样线测两焊盘，避免把大电流走线压降计入。

`ERR / NO SAMPLE` 表示采样失败或尚无读数，绝不表示 0V。Watch 查看 `adc_samples[0..3]`（VIN/VOUT/IIN/IOUT）的 `valid/code/pin_v` 及 `adc_error_count`。`valid` 仅表示转换完成，4095 等满量程读数仍需排查过量程；OLED 本身不读取 I2C ACK，屏幕没有显示也不是 ADC 初始化失败的证据。

## 初始化故障与验证范围

`tb_status.fault` 非零时关 PA1/PA2 并停机，可用 SWD 查看：

| fault | 原因 |
| --- | --- |
| 1 | MSP / 通用 Error_Handler |
| 2 / 3 / 4 | 电压档位 / HSE、PLL / 系统时钟切换失败 |
| 10 / 11 / 12 / 13 | DAC 初始化 / 通道配置 / 写值 / 启动失败 |
| 20 / 21 / 22 | ADC1 / ADC2 初始化 / 独立模式配置失败 |
| 23 / 24 | ADC1 / ADC2 自校准失败 |

初始化后单次 ADC 转换失败按通道显示 ERR 并累计次数，继续尝试下一组；不会关闭一个原本已经开启的输出，因为本测试从不打开输出。`fault=0` 不代表硬件测试通过。`tb_status.heartbeat` 在主循环更新。

软件验证命令：`python3 tests/run_module_logic.py`；它检查实际共用函数的 DAC 限幅、按键消抖/计时回绕、通道映射、弃样/均值以及 ADC 错误返回，不替代烧录、万用表和示波器实测。
