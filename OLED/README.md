# OLED 驱动说明（0.96 寸 / 4 针 I2C）

本目录是 **江协科技 0.96 寸 OLED 显示屏驱动程序 V2.0（2024.10.20）**，4 针脚 I2C 接口版本，用于 YLD_Cup 项目。
版权归江协科技所有（官网 jiangxiekeji.com），此处仅作使用说明。

| 文件 | 作用 |
| --- | --- |
| `OLED.h` / `OLED.c` | 驱动主体：I2C 时序、显存管理、字符/图形显示 |
| `OLED_Data.h` / `OLED_Data.c` | 字模与图像数据：ASCII 8×16、ASCII 6×8、汉字 16×16、自定义图像 |

---

## 1. 硬件与显存模型

### 1.1 引脚连接

驱动采用**软件模拟 I2C**（不是硬件 I2C 外设），引脚由 `OLED.c` 顶部「引脚配置」区的宏指定：

| 信号 | 引脚 | 配置 |
| --- | --- | --- |
| SCL | **PB3** | 开漏输出 `GPIO_MODE_OUTPUT_OD`，无上下拉，高速 |
| SDA | **PB4** | 开漏输出 `GPIO_MODE_OUTPUT_OD`，无上下拉，高速 |

对应代码（**换引脚只需改这 4 行宏，其余代码不用动**）：

```c
#define OLED_SCL_PORT		GPIOB
#define OLED_SCL_PIN		GPIO_PIN_3
#define OLED_SDA_PORT		GPIOB
#define OLED_SDA_PIN		GPIO_PIN_4
```

- I2C 从机地址固定为 `0x78`（7 位地址 0x3C 左移一位），见 `OLED_WriteCommand()` / `OLED_WriteData()`。
- 开漏输出依赖外部上拉电阻（OLED 模块一般自带），所以代码中 `Pull = GPIO_NOPULL`。
- PB3/PB4 在本工程 `.ioc` 里已配置为 GPIO 开漏输出（初始高）；`OLED_GPIO_Init()` 会再次使能 GPIOB 时钟并配成开漏。PB3/PB4 复位后是 JTAG 引脚，本工程只用 SWD（PA13/PA14），所以可作普通 GPIO。
- 若把 SCL / SDA 改到**不同端口**，记得同时在 `OLED_GPIO_Init()` 里使能另一个端口的时钟（HAL 的 `__HAL_RCC_GPIOx_CLK_ENABLE()` 是按端口写死的，没法用宏参数化）。

> **注意**：STM32G431 的 PA4 现在是内部 DAC1_OUT1（输出电压控制），所以 OLED 已从 PA4/PA5 挪到 PB3/PB4。PB3/PB4 复位后为 JTAG 引脚，SWD 模式下可作 GPIO；若以后改用 JTAG 调试，需要把 OLED 再挪走。

> **变更记录**：驱动原先接 PB8(SCL)/PB9(SDA)，后改 PA4/PA5；G431 迁移时 PA4 改为内部 DAC，OLED 最终改到 PB3/PB4。

### 1.2 显存模型

驱动采用「**显存 + 手动刷屏**」结构，所有显示/绘图函数只改内存，不动屏幕。

```c
uint8_t OLED_DisplayBuf[8][128];   // OLED.c:82，占 1KB RAM
```

- 屏幕 128×64 被分成 **8 页（Page）× 128 列**，每页高 8 像素。
- 数据格式：**纵向 8 点，低位在上**（bit0 是页内最上面那一点），先从左到右，再从上到下。
- 坐标系：左上角为 `(0, 0)`，X 向右 `0~127`，Y 向下 `0~63`。
- **因此所有函数完成后，必须调用 `OLED_Update()` 或 `OLED_UpdateArea()` 才会真正显示。**

---

## 2. 参数宏

| 宏 | 值 | 用途 |
| --- | --- | --- |
| `OLED_8X16` | 8 | 字体：宽 8 像素、高 16 像素（ASCII 与汉字均可显示） |
| `OLED_6X8` | 6 | 字体：宽 6 像素、高 8 像素（汉字退化为 `?`） |
| `OLED_UNFILLED` | 0 | 图形不填充 |
| `OLED_FILLED` | 1 | 图形填充 |

`OLED_Data.h` 中的字符集宏二选一（当前**启用 UTF8**）：

```c
#define OLED_CHARSET_UTF8      // 汉字索引 5 字节
//#define OLED_CHARSET_GB2312  // 汉字索引 3 字节
```

---

## 3. 函数一览

### 3.1 初始化与刷屏（最常用）

| 函数 | 说明 |
| --- | --- |
| `void OLED_Init(void)` | 初始化 GPIO + 写入一整套 SSD1306 配置命令 + 清屏刷屏。**使用前必须先调用** |
| `void OLED_Update(void)` | 把整个显存（8 页 × 128 列）写入屏幕，全屏刷新 |
| `void OLED_UpdateArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height)` | 只刷新指定矩形区域，比全屏刷新快得多，适合局部变量刷新。注意：若区域 Y 只覆盖某页的一部分，**同一页剩余部分会一起被刷新** |

### 3.2 显存控制

| 函数 | 说明 |
| --- | --- |
| `void OLED_Clear(void)` | 显存全部清 0（全屏清除） |
| `void OLED_ClearArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height)` | 清除指定矩形区域 |
| `void OLED_Reverse(void)` | 显存全部取反（黑白反转，可做闪烁效果） |
| `void OLED_ReverseArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height)` | 指定矩形区域取反 |

以上 4 个函数**只改显存**，需配合更新函数生效。

### 3.3 字符与数字显示

所有显示函数的 `X` / `Y` 均为**左上角坐标**，取值范围 `-32768~32767`，超出屏幕的部分自动裁剪（**可以利用负坐标做滚动/偏移效果**）。

| 函数 | 说明 |
| --- | --- |
| `void OLED_ShowChar(int16_t X, int16_t Y, char Char, uint8_t FontSize)` | 显示一个 ASCII 字符。`FontSize` 取 `OLED_8X16` / `OLED_6X8`；字符必须在 `' '`(0x20) 之后 |
| `void OLED_ShowString(int16_t X, int16_t Y, char *String, uint8_t FontSize)` | 显示字符串，**支持 ASCII 与中文混合**（自动按 UTF8/GB2312 拆字）。字宽固定：8×16 字体下，ASCII 宽 8、汉字宽 16；6×8 字体下中文显示为 `?` |
| `void OLED_ShowNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize)` | 十进制**无符号**整数。`Length` 为显示位数（0~10），不足位补前导 0，**不显示符号** |
| `void OLED_ShowSignedNum(int16_t X, int16_t Y, int32_t Number, uint8_t Length, uint8_t FontSize)` | 十进制**有符号**整数，**总是显示 `+`/`-` 号**，总宽 = `(Length + 1) × FontSize` |
| `void OLED_ShowHexNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize)` | 十六进制**大写**显示，`Length` 0~8，总宽 = `Length × FontSize` |
| `void OLED_ShowBinNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize)` | 二进制显示，`Length` 0~16 |
| `void OLED_ShowFloatNum(int16_t X, int16_t Y, double Number, uint8_t IntLength, uint8_t FraLength, uint8_t FontSize)` | 浮点数，**总是显示 `+`/`-` 号**，小数部分四舍五入；总宽 = `(IntLength + FraLength + 2) × FontSize`（1 位符号 + 1 位小数点） |
| `void OLED_ShowImage(int16_t X, int16_t Y, uint8_t Width, uint8_t Height, const uint8_t *Image)` | 显示位图。**会先清除该区域再写入**，因此是覆盖而非叠加；数据按页、列顺序存放 |
| `void OLED_Printf(int16_t X, int16_t Y, uint8_t FontSize, char *format, ...)` | 类似 `printf` 的格式化显示（内部 `vsprintf` 到 256 字节数组后交给 `OLED_ShowString`）。**注意缓冲区仅 256 字节且无边界检查，格式化结果不要过长** |

> 数字/浮点类函数底层都是循环调用 `OLED_ShowChar`，因此**同一行上字体宽度必须一致**，横坐标偏移由 `FontSize` 累加得到。

### 3.4 绘图函数

| 函数 | 说明 |
| --- | --- |
| `void OLED_DrawPoint(int16_t X, int16_t Y)` | 点亮一个像素（写 1，不影响同页其他像素） |
| `uint8_t OLED_GetPoint(int16_t X, int16_t Y)` | 读取像素状态，返回 1（点亮）/ 0（熄灭） |
| `void OLED_DrawLine(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1)` | 画线。水平/竖直线单独优化，斜线用 **Bresenham 算法**（纯整数运算，无浮点） |
| `void OLED_DrawRectangle(int16_t X, int16_t Y, uint8_t Width, uint8_t Height, uint8_t IsFilled)` | 矩形，`(X, Y)` 为左上角，支持填充 |
| `void OLED_DrawTriangle(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1, int16_t X2, int16_t Y2, uint8_t IsFilled)` | 三角形，三个顶点；填充用射线法（`OLED_pnpoly`）逐点判定 |
| `void OLED_DrawCircle(int16_t X, int16_t Y, uint8_t Radius, uint8_t IsFilled)` | 圆，`(X, Y)` 为圆心，Bresenham 算法 |
| `void OLED_DrawEllipse(int16_t X, int16_t Y, uint8_t A, uint8_t B, uint8_t IsFilled)` | 椭圆，`A` 为横向半轴、`B` 为纵向半轴 |
| `void OLED_DrawArc(int16_t X, int16_t Y, uint8_t Radius, int16_t StartAngle, int16_t EndAngle, uint8_t IsFilled)` | 圆弧 / 扇形。角度 `-180~180`，**水平向右为 0°，向下为正、向上为负，顺时针方向**；填充后即为扇形。`StartAngle < EndAngle` 画这两角之间，反之画另一侧 |

---

## 4. 内部函数（供上层调用，不建议在业务代码里直接用）

| 函数 | 层级 | 说明 |
| --- | --- | --- |
| `OLED_W_SCL` / `OLED_W_SDA` | 引脚 | 写 SCL / SDA 电平。**单片机太快导致通信异常时可在这两个函数里加延时** |
| `OLED_GPIO_Init` | 引脚 | 使能 GPIOB 时钟，把 PB3/PB4 按 `OLED_SCL_*` / `OLED_SDA_*` 宏配置成开漏输出并释放总线；初始化前有一段约 20 万次空循环等待 OLED 供电稳定 |
| `OLED_I2C_Start` / `OLED_I2C_Stop` | 协议 | I2C 起始 / 终止信号 |
| `OLED_I2C_SendByte` | 协议 | 发送 1 字节，MSB 先行，**额外的第 9 个时钟不处理应答信号** |
| `OLED_WriteCommand` | 协议 | 写命令：起始 + `0x78` + `0x00` + 命令 + 终止 |
| `OLED_WriteData` | 协议 | 连续写数据：起始 + `0x78` + `0x40` + N 字节 + 终止 |
| `OLED_SetCursor` | 硬件 | 设置页地址与列地址（`0xB0|Page`、`0x10|高 4 位`、`0x00|低 4 位`）。**若换成 1.3 寸 SH1106 屏（132 列，起始列接在第 2 列），需取消该函数里 `X += 2;` 的注释** |
| `OLED_Pow` | 工具 | 整数次方，供进制拆分使用 |
| `OLED_pnpoly` | 工具 | 判断点是否在多边形内（W. Randolph Franklin 的 pnpoly 算法） |
| `OLED_IsInAngle` | 工具 | 判断点是否落在指定角度区间内，供画圆弧使用 |

---

## 5. 字模数据扩展（`OLED_Data.c`）

| 数组 | 内容 |
| --- | --- |
| `OLED_F8x16[][16]` | ASCII 8×16 字模，从 `' '`(0x20) 开始，共 95 个可见字符 |
| `OLED_F6x8[][6]` | ASCII 6×8 字模，同上 |
| `OLED_CF16x16[]` | 汉字 16×16 字模，`ChineseCell_t` 结构（`Index` 字符 + `Data[32]`） |
| `Diode[]` | 示例图像（16×16 方框内一个二极管符号） |

**添加新汉字**：在 `OLED_CF16x16[]` 中按同样格式追加，字符集为 UTF8 时 `Index` 占 5 字节（3 字节汉字 + `'\0'`）。
数组**末尾必须以空字符串 `""` 的默认图形作为哨兵**（一个方框内问号），`OLED_ShowString` 靠它判断「字模不存在」，务必不要删掉或挪动位置。

**添加新图像**：在 `OLED_Data.c` 末尾按 `Diode[]` 的格式定义数组，并在 `OLED_Data.h` 中用 `extern` 声明，然后用 `OLED_ShowImage()` 显示。
图像数据按页存放，`Height` 不是 8 的整数倍时，最后一个字节的多余位会被忽略。

---

## 6. 典型用法

```c
#include "OLED.h"

/* main 中，MX_GPIO_Init() 之后 */
OLED_Init();                          // 初始化（含清屏）

/* 上电后只刷一次的内容 */
OLED_ShowString(0, 0, "YLD Cup", OLED_8X16);
OLED_ShowString(0, 16, "电压:", OLED_8X16);
OLED_DrawRectangle(0, 0, 128, 64, OLED_UNFILLED);
OLED_Update();                        // 全屏刷新

/* 循环内：局部更新，避免全屏刷新拖慢主循环 */
while (1)
{
    OLED_ShowFloatNum(48, 32, INA_V_IN, 1, 3, OLED_8X16);   // +1.234
    OLED_UpdateArea(48, 32, 48, 16);                       // 参数与上面显示区域一致
    HAL_Delay(100);
}
```

`OLED_ShowFloatNum(48, 32, v, 1, 3, OLED_8X16)` 占用区域计算：宽 `(1 + 3 + 2) × 8 = 48`，高 `16`。

---

## 7. 注意事项汇总

1. **必须调用 `OLED_Init()`**，放在 `MX_GPIO_Init()` 之后的用户代码区即可。PB3/PB4 在 SWD 调试下可作 GPIO，驱动的 GPIO 初始化会自行配置引脚并使能 GPIOB 时钟。
2. **显示不等于刷屏**：所有显示/绘图函数只写显存，必须调用 `OLED_Update()` / `OLED_UpdateArea()`。
3. **`OLED_UpdateArea()` 的粒度是页**，Y 不落在页边界时会连带刷新同页其他内容，局部刷新时注意别把相邻内容刷掉。
4. **`OLED_ShowImage()` 是覆盖式的**（内部先 `OLED_ClearArea`），在已有内容上叠加会擦掉原内容；需要叠加请用绘图函数逐点画。
5. **`OLED_Printf()` 缓冲区 256 字节且用 `vsprintf` 不做边界检查**，长字符串会溢出栈，谨慎使用。
6. **浮点显示固定带 `+` 号**，不需要符号时用 `OLED_ShowNum()` 拆整数/小数部分自行显示。
7. 软件 I2C **无应答检测、无延时**，若出现花屏/认不到屏，优先在 `OLED_W_SCL()` / `OLED_W_SDA()` 里加少量延时或降低主频。
8. **换引脚**：只改 `OLED.c` 引脚配置区的 `OLED_SCL_PORT/PIN`、`OLED_SDA_PORT/PIN` 四个宏即可。**换 1.3 寸屏（SH1106）**需改 `OLED_SetCursor()`；**换硬件 I2C** 只需重写 `OLED_W_SCL()` / `OLED_W_SDA()` 两个函数。
9. 本工程 `CMakeLists.txt` 已加入 `OLED/OLED.c`、`OLED/OLED_Data.c` 及头文件搜索路径，无需额外配置。
