# TEST — 独立采样标定固件

本目录是一个**独立固件**：单独编译、单独烧录，整机上只跑这个标定工具，不依赖也不改动主程序。

用途：在本板上实测**电流采样链路**（采样电阻 R6/R1 × INA199 增益）和**电压分压系数**（Vin / Vout 两路的 ÷7 网络），把结果填回 `Core/Src/main.c` 顶部的宏。界面实时显示 4 个通道的 12 位 ADC 码值和引脚原始电压，你用仪器提供已知的输入/输出电压电流，对照读数即可反推系数。

硬件（STM32G431CBT6）：Vin=PA7/ADC2_IN4，Vout=PA6/ADC2_IN3，Iin=PB1/ADC1_IN12，Iout=PB0/ADC1_IN15，DAC=PA4/DAC1_OUT1，OLED=PB3/PB4，输出 MOS=PA1。

## 1. 编译与烧录（独立固件，不碰主程序）

- **CLion**：复制一份运行/调试配置，把 **Target 换成 `YLD_Cup_Calib`**，构建下载即可；Target 选回 `YLD_Cup` 就是正常主固件。
- **命令行**：

```bash
cmake --build --preset Debug --target YLD_Cup_Calib   # 产物 build/Debug/TEST/YLD_Cup_Calib.elf
```

- 烧录方式与平时相同，只是烧的是 `YLD_Cup_Calib.elf`；测完把主固件烧回去即恢复正常功能。
- 应用逻辑全部在单个文件 `calib_main.c` 里（自带 main、时钟与全部外设初始化），只复用板级文件和 OLED 驱动；主固件源文件零改动。

## 2. 界面与按键

```
CALIB MODE (RAW)      <- 标题
VIN  2831 +2.2814V    <- 标签 | 12 位码值 | 引脚电压 (16 次均值, 4 位小数)
VOUT xxxx +x.xxxxV
IIN  xxxx +x.xxxxV
IOUT xxxx +x.xxxxV
OUT OFF  SET 8.0V     <- 输出通断 / 开环 DAC 目标 (不进闭环, 无保护)
K3:OUT K1/K2:SET
AVG=16 SAMPLES
```

- **K3（B13）**：输出 MOS 开 / 关（打开前自动把 DAC 按目标预置好）；**K1（B15）/ K2（B14）**：目标 ±1V（5~15V），开输出时立即生效。
- 标定模式没有保护逻辑：带负载调试前请把电源限流、电子负载挡位先设好。

## 3. 标定方法

屏上的 `raw` 是 ADC 引脚电压，不是已经换算后的输入/输出电压电流。两路电流的 REF 继续接地，不新增偏置电路，不在上电时自动归零。

### 3.1 电压通道

新图 R4/R8=60kΩ、R2/R3=10kΩ，理论分压比为 7。用台表读数 `ref` 与屏上 `raw` 对比：

```text
VIN_ADC_SCALE = Vin台表读数 / VIN原始电压
VOUT_ADC_SCALE = Vout相对系统地读数 / VOUT原始电压
```

在多个电压点复核。Q3 是负端开关，PA6 测正端对系统地；负载端子之间的实际压差还要扣除 Q3 压降，不能混用两种参考点。

### 3.2 电流通道 两个非零点分别拟合

理论链路：20mΩ × INA199A1 的 50 倍增益 = 1V/A，即 0.2A → 0.2V、2A → 2V、2.3A → 2.3V。这不是已经标定的精度保证。

输入、输出通道各做一轮：提供两个准确的非零电流 `I1`、`I2`，记录对应原始电压 `raw1`、`raw2`。输出可先选 0.2A、2A；输入应覆盖实际最低工作电流，仪器读数以输入电流为准，不能把输出电流直接当输入电流。

```text
k = (I2 - I1) / (raw2 - raw1)     单位 A/V
b = I1 - k * raw1                单位 A
电流 = raw * k + b
GAIN_TRIM = k * SHUNT_OHM * INA199_GAIN
OFFSET_A = b
```

例如 I1=0.200A、raw1=0.205V，I2=2.000A、raw2=2.005V，则 k=1.000A/V、b=-0.005A。20mΩ、50 倍增益下，填写对应通道 `GAIN_TRIM=1.000f`、`OFFSET_A=-0.005f`。这些只是算例，不能替代实测。

用 0.1A（或实际最低输入电流）、0.5A、1A、1.5A 等点复核，并在热机后重复。REF 接地时零附近可能输出限幅，不能只减去零电流读数；输入侧即使空载也有变换器静态电流。

## 4. 填回主程序

所有参数位于 `Core/Src/main.c` 的 USER CODE PD 区：

| 测量项目 | 参数 |
| --- | --- |
| 输入/输出电压比例 | `VIN_ADC_SCALE` / `VOUT_ADC_SCALE` |
| 输入 R6 / 输出 R1 阻值，默认 0.020Ω | `IIN_SHUNT_OHM` / `IOUT_SHUNT_OHM` |
| INA199A1 增益，默认 50 | `INA199_GAIN` |
| 两路增益修正，默认 1 | `IIN_GAIN_TRIM` / `IOUT_GAIN_TRIM` |
| 两路电流截距，默认 0A | `IIN_OFFSET_A` / `IOUT_OFFSET_A` |

主程序采用 `I = raw * GAIN_TRIM / (SHUNT_OHM * INA199_GAIN) + OFFSET_A`，最后将负电流钳到 0。输入与输出需各自填写，不能共用一组实测系数。填完重新编译并烧录主固件 `YLD_Cup`。

## 5. 注意事项

- 标定固件已同步 PB15/PB14/PB13 按键及 R13=2.2kΩ 的 DAC 理论关系，8V 目标约 1.996V。
- 采样条件与主固件一致：单端、640.5 周期、切换后弃首样本；本工具取16次均值，主程序取4次。ADC2_IN4=Vin、ADC2_IN3=Vout、ADC1_IN12=Iin、ADC1_IN15=Iout。
- 本工具没有主固件的过流/欠压保护或 ADC 故障锁存；使用电源限流和合适负载进行标定。主程序检测 ADC 调用失败会关输出并显示故障，但本工具失败时可能显示0，不能据此认为电流为0。
- 步进调压是开环预置；16V 输入能否带载输出15V仍取决于功率级压差，不由软件公式保证。
