#ifndef CC_CONFIG_H
#define CC_CONFIG_H

/* 与主程序共用实测换算，避免调参时使用另一套电流读数。 */
#include "measurement_calibration.h"
#define CC_VIN_SCALE        CAL_VIN_ADC_SCALE
#define CC_VOUT_SCALE       CAL_VOUT_ADC_SCALE
#define CC_DAC_A            17.07091f
#define CC_DAC_B            4.545455f

#define CC_PERIOD_MS        10U
#define CC_MAX_GAP_MS       25U
#define CC_DAC_MAX          3.1f
#define CC_VOLT_LIMIT       12.0f
#define CC_DAC_MIN          ((CC_DAC_A - CC_VOLT_LIMIT) / CC_DAC_B)
#define CC_PRECHARGE_MS     400U
#define CC_PRECHARGE_MAX_MS 3000U
#define CC_PRECHARGE_V      4.0f
#define CC_INPUT_MIN_V      12.5f
#define CC_START_V          13.0f
#define CC_CURRENT_MAX_A    2.3f
#define CC_CURRENT_MARGIN   0.35f
#define CC_TARGET_MIN       0.4f
#define CC_TARGET_MAX       1.5f

#define CC_SEEK_STEP_V      0.005f
#define CC_SEEK_HOLD_MS     100U
#define CC_SEEK_TIMEOUT_MS  60000U
#define CC_RELAY_V          0.05f /* DAC 对称扰动半幅，单位 V */
#define CC_HYST_A           0.003f
#define CC_HALF_TIMEOUT_MS  5000U
#define CC_DISCARD_CYCLES   2U
#define CC_CYCLES           6U
#define CC_MIN_CYCLE_MS     (12U * CC_PERIOD_MS)
#define CC_MIN_AMPLITUDE_A  0.015f /* 至少为迟滞的 5 倍 */
#define CC_MAX_SPREAD       0.20f /* 周期、半幅相对均值的最大偏差 */
#define CC_DAC_SLEW_V_S     0.5f
#define CC_VERIFY_STEP_A    0.1f
#define CC_VERIFY_STEP_MS   2000U
#define CC_VERIFY_END_MS    6000U
#define CC_TRACE_SIZE       512U

#endif
