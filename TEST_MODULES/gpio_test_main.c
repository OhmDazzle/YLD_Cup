/* 按键 / 蜂鸣器：三个键分别发出 1/2/3 声；不初始化 ADC/DAC。 */
#include "test_board.h"
#include "OLED.h"

volatile uint32_t gpio_press_count[3];
volatile unsigned gpio_last_key;

int main(void)
{
  uint32_t beep_started = 0U, drawn;
  unsigned beep_count = 0U;
  TB_Init();
  OLED_Init();
  drawn = HAL_GetTick() - 100U;
  while (1) {
    uint8_t events = TB_KeysPoll();
    uint32_t now = HAL_GetTick();
    for (unsigned i = 0; i < 3U; ++i) {
      if (events & (1U << i)) {
        ++gpio_press_count[i];
        gpio_last_key = i + 1U;
        beep_count = i + 1U;
        beep_started = now;
      }
    }
    /* 100ms 响 / 100ms 停，按住不连响，最后一个新按键覆盖原序列。 */
    uint32_t elapsed = (uint32_t)(now - beep_started);
    if (elapsed >= beep_count * 200U) beep_count = 0U;
    TB_Buzzer(beep_count != 0U && elapsed % 200U < 100U);
    if ((uint32_t)(now - drawn) >= 100U) {
      drawn = now;
      OLED_Clear();
      OLED_ShowString(0, 0, "KEY / BUZZER TEST", OLED_6X8);
      for (unsigned i = 0; i < 3U; ++i) {
        int16_t y = (int16_t)(8U + 8U * i);
        OLED_ShowChar(0, y, 'K', OLED_6X8);
        OLED_ShowNum(6, y, i + 1U, 1, OLED_6X8);
        OLED_ShowString(18, y, (tb_status.keys_down & (1U << i)) ? "DOWN" : "UP", OLED_6X8);
        OLED_ShowNum(60, y, gpio_press_count[i] % 1000000U, 6, OLED_6X8);
      }
      OLED_ShowString(0, 32, "LAST K", OLED_6X8);
      OLED_ShowNum(36, 32, gpio_last_key, 1, OLED_6X8);
      OLED_ShowString(0, 40, "K1/2/3 = 1/2/3 BEEP", OLED_6X8);
      OLED_ShowString(0, 48, "PA2:H=ON PA1:L=OFF", OLED_6X8);
      OLED_ShowString(0, 56, "Q3 OFF / LOGIC 5V", OLED_6X8);
      OLED_Update();
    }
    HAL_Delay(2U);
  }
}
