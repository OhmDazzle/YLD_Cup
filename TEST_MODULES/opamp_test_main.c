/* 万用表测试：仅接逻辑 5V，断开 13~20V 主电源和负载。见 README。 */
#include "test_board.h"
#include "OLED.h"

static const float levels[] = {0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f};
volatile unsigned opamp_level = 3U;
volatile bool opamp_auto;

int main(void)
{
  uint32_t changed, drawn;
  TB_Init();
  TB_DacInit(levels[opamp_level]);
  OLED_Init();
  changed = HAL_GetTick();
  drawn = changed - 100U;
  while (1) {
    uint8_t keys = TB_KeysPoll();
    uint32_t now = HAL_GetTick();
    unsigned next = opamp_level;
    if (keys & TB_K3) { opamp_auto = !opamp_auto; changed = now; }
    if (keys & (TB_K1 | TB_K2)) {
      opamp_auto = false;
      if ((keys & TB_K1) && next < 5U) ++next;
      if ((keys & TB_K2) && next > 0U) --next;
    }
    if (opamp_auto && (uint32_t)(now - changed) >= 5000U)
      next = (next + 1U) % 6U;
    if (next != opamp_level) {
      opamp_level = next;
      TB_DacWrite(levels[next]);
      changed = now;
    }
    if ((uint32_t)(now - drawn) >= 100U) {
      drawn = now;
      OLED_Clear();
      OLED_ShowString(0, 0, "DAC / OPA320 TEST", OLED_6X8);
      OLED_ShowString(0, 8, "CMD:", OLED_6X8);
      OLED_ShowFloatNum(24, 8, tb_status.dac_v, 1, 3, OLED_6X8);
      OLED_ShowString(72, 8, "V", OLED_6X8);
      OLED_ShowString(0, 16, "CODE:", OLED_6X8);
      OLED_ShowNum(30, 16, tb_status.dac_code, 4, OLED_6X8);
      OLED_ShowString(0, 24, "DMM:PA4 / U2:3,1,4", OLED_6X8);
      OLED_ShowString(0, 32, "U2:2=GND 5=3.3V", OLED_6X8);
      OLED_ShowString(0, 40, opamp_auto ? "AUTO 5s    Q3 OFF" : "MANUAL     Q3 OFF", OLED_6X8);
      OLED_ShowString(0, 48, "K1:+ K2:- K3:AUTO", OLED_6X8);
      OLED_ShowString(0, 56, "CMD IS NOT MEASURED", OLED_6X8);
      OLED_Update();
    }
    HAL_Delay(2U);
  }
}
