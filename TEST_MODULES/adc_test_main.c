/* 四路采样独立测试：不闭合输出 MOS，显示真实 ADC 引脚电压和未标定换算。 */
#include "test_board.h"
#include "OLED.h"

volatile TB_AdcSample adc_samples[4]; /* VIN,VOUT,IIN,IOUT；可在 Watch 查看。 */
volatile uint32_t adc_error_count;
volatile bool adc_hold, adc_physical;

int main(void)
{
  static char *names[4] = {"VIN", "VOUT", "IIN", "IOUT"};
  uint32_t sampled, drawn;
  TB_Init();
  TB_DacInit(2.0f); /* 固定命令，避免 PA4 浮空；ADC 测试不扫 DAC。 */
  TB_AdcInit();
  OLED_Init();
  sampled = drawn = HAL_GetTick() - 100U;
  while (1) {
    uint8_t keys = TB_KeysPoll();
    uint32_t now = HAL_GetTick();
    if (keys & TB_K1) adc_physical = !adc_physical;
    if (keys & TB_K2) adc_error_count = 0U;
    if (keys & TB_K3) adc_hold = !adc_hold;
    if (!adc_hold && (uint32_t)(now - sampled) >= 100U) {
      sampled = now;
      for (unsigned i = 0; i < 4U; ++i) {
        TB_AdcSample value = {0};
        if (!TB_AdcRead(i, &value)) ++adc_error_count;
        adc_samples[i] = value;
      }
    }
    if ((uint32_t)(now - drawn) >= 100U) {
      drawn = now;
      OLED_Clear();
      OLED_ShowString(0, 0, adc_physical ? "ADC EST V/A (UNCAL)" : "ADC RAW / PIN VOLTS", OLED_6X8);
      for (unsigned i = 0; i < 4U; ++i) {
        int16_t y = (int16_t)(8U + 8U * i);
        TB_AdcSample value = adc_samples[i];
        OLED_ShowString(0, y, names[i], OLED_6X8);
        if (!value.valid) {
          OLED_ShowString(30, y, "ERR / NO SAMPLE", OLED_6X8);
        } else {
          float scale = adc_physical && i < 2U ? 7.0f : 1.0f;
          OLED_ShowNum(24, y, value.code, 4, OLED_6X8);
          OLED_ShowFloatNum(54, y, value.pin_v * scale, 2, 3, OLED_6X8);
          OLED_ShowChar(102, y, adc_physical && i >= 2U ? 'A' : 'V', OLED_6X8);
        }
      }
      OLED_ShowString(0, 40, adc_hold ? "HOLD       Q3 OFF" : "LIVE AVG16 Q3 OFF", OLED_6X8);
      OLED_ShowString(0, 48, "K1:UNIT K2:CLR ERR", OLED_6X8);
      OLED_ShowString(0, 56, "K3:HOLD E:", OLED_6X8);
      OLED_ShowNum(60, 56, adc_error_count % 1000000U, 6, OLED_6X8);
      OLED_Update();
    }
    HAL_Delay(2U);
  }
}
