/* 独立恒流调参固件：只有显式按键启动后才允许打开输出。 */
#include "cc_pid.h"
#include "test_board.h"
#include "OLED.h"
#include <math.h>

typedef struct {
  uint32_t ms;
  float iout, setpoint, dac;
  uint32_t state;
} CCTrace;

/* Watch：实际选中系数在 cc_status.gains 中，单位见 README。
 * 手动试系数：在 IDLE/READY 修改 cc_manual、cc_manual_bias，置 cc_apply_manual=1。
 * 调试暂停会延长实际控制间隔，恢复时会触发 TIMING 保护。 */
volatile CCTuner cc_status;
volatile CCMeasurement cc_measurement;
volatile CCGains cc_manual;
volatile float cc_manual_bias = (CC_DAC_A - 8.0f) / CC_DAC_B;
volatile uint32_t cc_apply_manual;
volatile CCTrace cc_trace[CC_TRACE_SIZE];
volatile uint32_t cc_trace_count;
volatile uint32_t cc_max_gap_ms;
static CCTuner tuner;

static bool sample(CCMeasurement *m)
{
  TB_AdcSample a[4];
  for (unsigned i = 0U; i < 4U; ++i) {
    if (!TB_AdcRead(i, &a[i])) { m->valid = false; return false; }
    /* HAL 正常但高端贴轨同样不能当正常测量继续调参。 */
    if (a[i].code >= 4090U) { m->valid = false; return false; }
  }
  *m = (CCMeasurement){
    a[0].pin_v*CC_VIN_SCALE, a[1].pin_v*CC_VOUT_SCALE,
    fmaxf(0.0f, a[2].pin_v*CC_IIN_SCALE + CC_IIN_OFFSET),
    fmaxf(0.0f, a[3].pin_v*CC_IOUT_SCALE + CC_IOUT_OFFSET), true
  };
  return true;
}

static void apply_output(void)
{
  /* 关断优先于写 DAC；DAC 写失败时板级错误处理也会关 PA1。 */
  if (!tuner.output_on) HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_RESET);
  TB_DacWrite(tuner.dac);
  if (tuner.output_on) HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_SET);
  TB_Buzzer(tuner.state == CC_FAULT);
}

static void number(unsigned y, char *label, float v, unsigned decimals)
{
  OLED_ShowString(0, y, label, OLED_6X8);
  OLED_ShowFloatNum(30, y, v, 3, decimals, OLED_6X8);
}

static void draw(CCMeasurement m)
{
  OLED_Clear();
  if (CC_Active(&tuner)) {
    OLED_ShowString(0, 0, "CC TEST RUNNING", OLED_6X8);
    number(8, "ISET", tuner.target, 3);
    OLED_ShowString(0, 24, "DISPLAY PAUSED", OLED_6X8);
    OLED_ShowString(0, 32, "FOR 10ms SAMPLING", OLED_6X8);
    OLED_ShowString(0, 48, "K3:STOP OUTPUT", OLED_6X8);
    OLED_ShowString(0, 56, "WATCH: cc_status", OLED_6X8);
  } else if (tuner.state == CC_READY) {
    OLED_ShowString(0, 0, tuner.manual_result ? "M" : (tuner.use_pid ? "PID" : "PI"), OLED_6X8);
    OLED_ShowString(24, 0, "x", OLED_6X8);
    OLED_ShowFloatNum(30, 0, tuner.scale, 1, 2, OLED_6X8);
    OLED_ShowString(72, 0, "Q3 OFF", OLED_6X8);
    number(8, "KP", tuner.gains.kp, 4);
    number(16, "KI", tuner.gains.ki, 4);
    number(24, "KD", tuner.gains.kd, 4);
    if (tuner.verify_valid) {
      number(32, "OS%", tuner.overshoot_pct, 1);
      number(40, "ERR", tuner.final_error, 4);
    } else {
      number(32, "TU/s", tuner.tu, 3);
      number(40, "KU", tuner.ku, 3);
    }
    OLED_ShowString(0, 48, "K1:TYPE K2:SCALE", OLED_6X8);
    OLED_ShowString(0, 56, "K3:TEST HOLD:RESET", OLED_6X8);
  } else if (tuner.state == CC_FAULT) {
    OLED_ShowString(0, 0, "CC FAULT  Q3 OFF", OLED_6X8);
    OLED_ShowString(0, 16, (char *)CC_ErrorText(tuner.error), OLED_6X8);
    number(24, "VIN", m.vin, 2);
    number(32, "IOUT", m.iout, 3);
    OLED_ShowString(0, 56, "K3:ACK TO IDLE", OLED_6X8);
  } else {
    OLED_ShowString(0, 0, "CC AUTOTUNE  Q3 OFF", OLED_6X8);
    number(8, "ISET", tuner.target, 3);
    number(16, "VIN", m.vin, 2);
    number(24, "VOUT", m.vout, 2);
    OLED_ShowString(0, 40, "USE RESISTIVE LOAD", OLED_6X8);
    OLED_ShowString(0, 48, "K1:+0.1 K2:-0.1", OLED_6X8);
    OLED_ShowString(0, 56, "K3:AUTOTUNE", OLED_6X8);
  }
  OLED_Update();
}

static void start(uint32_t now, bool verify)
{
  if (CC_Start(&tuner, now, verify)) {
    cc_trace_count = cc_max_gap_ms = 0U;
    cc_manual = tuner.gains;
  }
}

int main(void)
{
  TB_Init();
  TB_DacInit(CC_DAC_MAX);
  TB_AdcInit();
  OLED_Init();
  CC_Init(&tuner, 1.0f);
  CCMeasurement m = {0};
  uint32_t sampled = HAL_GetTick(), drawn = sampled - 200U, pressed = 0U;
  bool func_was_down = false, func_consumed = false, was_active = false;
  while (1) {
    uint8_t events = TB_KeysPoll();
    uint32_t now = HAL_GetTick();
    bool down = (tb_status.keys_down & TB_K3) != 0U;
    if (down && !func_was_down) {
      pressed = now; func_consumed = false;
      if (CC_Active(&tuner)) {
        CC_Stop(&tuner); apply_output(); func_consumed = true;
      }
    }
    if (down && !func_consumed && (uint32_t)(now - pressed) >= 800U) {
      CC_Init(&tuner, tuner.target); func_consumed = true;
      apply_output();
    }
    if (!down && func_was_down && !func_consumed) {
      if (tuner.state == CC_FAULT) CC_Init(&tuner, tuner.target);
      else if (tuner.state == CC_READY) start(now, true);
      else if (tuner.state == CC_IDLE) start(now, false);
    }
    func_was_down = down;
    if (tuner.state == CC_IDLE) {
      if (events & TB_K1) tuner.target = fminf(CC_TARGET_MAX, tuner.target + 0.1f);
      if (events & TB_K2) tuner.target = fmaxf(CC_TARGET_MIN, tuner.target - 0.1f);
    } else if (tuner.state == CC_READY) {
      if (events & TB_K1) CC_SelectGains(&tuner, !tuner.use_pid, tuner.scale);
      if (events & TB_K2) CC_SelectGains(&tuner, tuner.use_pid,
          tuner.scale < 0.3f ? 0.5f : (tuner.scale < 0.6f ? 1.0f : 0.25f));
    }
    if ((tuner.state == CC_IDLE || tuner.state == CC_READY) && cc_apply_manual != 0U) {
      CCGains g = cc_manual;
      cc_apply_manual = 0U;
      (void)CC_LoadManual(&tuner, g, cc_manual_bias);
    }

    /* 运行期间不刷新阻塞式软件 I2C，避免扰动采样/控制周期。 */
    bool active = CC_Active(&tuner);
    if ((!active && (uint32_t)(now - drawn) >= 200U) || (active && !was_active)) {
      draw(m); drawn = HAL_GetTick();
      if (!tuner.output_on) sampled = drawn;
    }
    was_active = active;
    now = HAL_GetTick();
    if ((uint32_t)(now - sampled) < CC_PERIOD_MS) { HAL_Delay(1U); continue; }
    sampled = now;
    /* 在任何可能阻塞的 ADC 调用之前检查前一次采样完成时间。 */
    if (tuner.output_on && (uint32_t)(now - tuner.previous_tick) > CC_MAX_GAP_MS) {
      CC_Fail(&tuner, CC_TIMING);
      apply_output();
    }
    if (!sample(&m)) CC_Fail(&tuner, CC_ADC);
    now = HAL_GetTick();
    if (tuner.output_on) {
      uint32_t gap = now - tuner.previous_tick;
      if (gap > cc_max_gap_ms) cc_max_gap_ms = gap;
    }
    bool record = CC_Active(&tuner);
    CC_Update(&tuner, now, m);
    apply_output();
    cc_measurement = m;
    cc_status = tuner;
    if (record) {
      uint32_t count = cc_trace_count;
      cc_trace[count % CC_TRACE_SIZE] = (CCTrace){
        now, m.iout, tuner.setpoint, tb_status.dac_v, (uint32_t)tuner.state
      };
      cc_trace_count = count + 1U;
    }
  }
}
