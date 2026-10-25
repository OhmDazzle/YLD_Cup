#include "cc_pid.h"
#include <math.h>
#include <string.h>

static float clamp(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

bool CC_Active(const CCTuner *t)
{
  return t->state == CC_PRECHARGE || t->state == CC_SEEK ||
         t->state == CC_RELAY || t->state == CC_VERIFY;
}

void CC_Init(CCTuner *t, float target)
{
  memset(t, 0, sizeof(*t));
  t->target = clamp(isfinite(target) ? target : 1.0f, CC_TARGET_MIN, CC_TARGET_MAX);
  t->dac = CC_DAC_MAX;
  t->scale = 0.25f;
  t->settling_ms = -1.0f;
}

void CC_Stop(CCTuner *t)
{
  /* 中断验证不产生有效指标；保留之前识别的系数供再次验证。 */
  if (t->state == CC_VERIFY || t->state == CC_PRECHARGE) t->verify_valid = false;
  t->output_on = false;
  t->dac = CC_DAC_MAX;
  t->state = t->valid_result ? CC_READY : CC_IDLE;
}

void CC_Fail(CCTuner *t, CCError error)
{
  t->output_on = false;
  t->dac = CC_DAC_MAX;
  t->verify_valid = false;
  t->error = error;
  t->state = CC_FAULT;
}

void CC_SelectGains(CCTuner *t, bool pid, float scale)
{
  t->use_pid = pid;
  t->scale = clamp(isfinite(scale) ? scale : 0.25f, 0.1f, 1.0f);
  CCGains g = pid ? t->pid : t->pi;
  t->gains = (CCGains){g.kp*t->scale, g.ki*t->scale, g.kd*t->scale};
  t->verify_valid = false;
}

bool CC_LoadManual(CCTuner *t, CCGains gains, float bias)
{
  if (CC_Active(t) || t->state == CC_FAULT || !isfinite(gains.kp) ||
      !isfinite(gains.ki) || !isfinite(gains.kd) || gains.kp <= 0.0f ||
      gains.ki < 0.0f || gains.kd < 0.0f || !isfinite(bias) ||
      bias < CC_DAC_MIN || bias > CC_DAC_MAX) return false;
  t->bias = bias;
  t->pi = t->pid = gains;
  t->pi.kd = 0.0f;
  t->valid_result = t->manual_result = true;
  t->ku = t->tu = 0.0f;
  t->error = CC_OK;
  t->state = CC_READY;
  CC_SelectGains(t, gains.kd > 0.0f, 1.0f);
  return true;
}

bool CC_Start(CCTuner *t, uint32_t now, bool verify)
{
  if (CC_Active(t) || t->state == CC_FAULT) return false;
  if (verify && (!t->valid_result || !isfinite(t->gains.kp) ||
      !isfinite(t->gains.ki) || !isfinite(t->gains.kd) ||
      t->gains.kp <= 0.0f || t->gains.ki < 0.0f || t->gains.kd < 0.0f)) return false;
  if (!verify) t->valid_result = t->manual_result = false;
  t->verify_valid = false;
  t->verify_pending = verify;
  t->output_on = false;
  t->error = CC_OK;
  t->dac = CC_DAC_MAX;
  t->setpoint = t->target;
  t->started = t->previous_tick = t->changed = now;
  t->cycles = t->accepted = 0U;
  t->state = CC_PRECHARGE;
  return true;
}

static bool measurement_ok(CCTuner *t, CCMeasurement m)
{
  if (!m.valid) { CC_Fail(t, CC_ADC); return false; }
  if (!isfinite(m.vin) || !isfinite(m.vout) || !isfinite(m.iin) || !isfinite(m.iout)) {
    CC_Fail(t, CC_BAD_NUMBER); return false;
  }
  if (m.vin < (t->output_on ? CC_INPUT_MIN_V : CC_START_V)) {
    CC_Fail(t, CC_UVP); return false;
  }
  if (m.iout > CC_CURRENT_MAX_A || m.iout > t->target + CC_CURRENT_MARGIN) {
    CC_Fail(t, CC_OCP); return false;
  }
  if (m.vout > CC_VOLT_LIMIT) { CC_Fail(t, CC_OVP); return false; }
  return true;
}

static void finish_relay(CCTuner *t)
{
  float period = 0.0f, amp = 0.0f;
  for (unsigned i = 0U; i < CC_CYCLES; ++i) {
    period += t->periods[i]; amp += t->amplitudes[i];
  }
  period /= CC_CYCLES; amp /= CC_CYCLES;
  for (unsigned i = 0U; i < CC_CYCLES; ++i) {
    if (fabsf(t->periods[i] - period) > CC_MAX_SPREAD*period ||
        fabsf(t->amplitudes[i] - amp) > CC_MAX_SPREAD*amp) {
      CC_Fail(t, CC_NOISY); return;
    }
  }
  t->tu = period * 0.001f;
  t->amplitude = amp;
  /* 理想继电器描述函数近似。迟滞影响由 a>=5h 和占空比检查限制，
   * Ku/Tu 是估计而非被严格识别的临界点，结果必须阶跃验证。 */
  t->ku = 4.0f * CC_RELAY_V / (3.14159265358979323846f * amp);
  t->pi = (CCGains){0.45f*t->ku, 0.54f*t->ku/t->tu, 0.0f};
  t->pid = (CCGains){0.60f*t->ku, 1.20f*t->ku/t->tu, 0.075f*t->ku*t->tu};
  t->valid_result = true;
  CC_SelectGains(t, false, 0.25f);
  CC_Stop(t);
}

static void relay_step(CCTuner *t, uint32_t now, float current)
{
  if ((uint32_t)(now - t->changed) > CC_HALF_TIMEOUT_MS) {
    CC_Fail(t, CC_TIMEOUT); return;
  }
  t->peak = fmaxf(t->peak, current);
  t->trough = fminf(t->trough, current);
  if (t->high && current >= t->target + CC_HYST_A) {
    if (t->anchored) {
      uint32_t period = now - t->cycle_start;
      float amp = (t->peak - t->trough) * 0.5f;
      float duty = (float)(now - t->high_start) / (float)period;
      ++t->cycles;
      if (t->cycles > CC_DISCARD_CYCLES) {
        if (period < CC_MIN_CYCLE_MS) { CC_Fail(t, CC_TOO_FAST); return; }
        if (amp < CC_MIN_AMPLITUDE_A || amp < 5.0f*CC_HYST_A) {
          CC_Fail(t, CC_NOISY); return;
        }
        if (duty < 0.2f || duty > 0.8f) { CC_Fail(t, CC_ASYMMETRY); return; }
        t->periods[t->accepted] = (float)period;
        t->amplitudes[t->accepted++] = amp;
        if (t->accepted == CC_CYCLES) { finish_relay(t); return; }
      }
    }
    t->anchored = true;
    t->cycle_start = now;
    t->peak = t->trough = current;
    t->high = false;
    t->changed = now;
  } else if (!t->high && current <= t->target - CC_HYST_A) {
    t->high = true;
    t->high_start = t->changed = now;
  }
  /* 此板负增益：需要增加电流时降低 DAC。 */
  t->dac = t->bias + (t->high ? -CC_RELAY_V : CC_RELAY_V);
}

static void verify_step(CCTuner *t, uint32_t now, float current, float dt)
{
  uint32_t elapsed = now - t->started;
  t->setpoint = t->target + (elapsed >= CC_VERIFY_STEP_MS ? CC_VERIFY_STEP_A : 0.0f);
  float e = t->setpoint - current;
  float tau = fmaxf(0.03f, t->gains.kd / t->gains.kp / 10.0f);
  t->derivative += dt/(tau + dt) * ((current - t->previous_i)/dt - t->derivative);
  t->previous_i = current;
  float candidate_i = t->gains.ki > 0.0f ? t->integral + t->gains.ki*e*dt : 0.0f;
  float raw = t->bias - t->gains.kp*e - candidate_i + t->gains.kd*t->derivative;
  if (!isfinite(raw) || !isfinite(candidate_i) || !isfinite(t->derivative)) {
    CC_Fail(t, CC_BAD_NUMBER); return;
  }
  float lo = fmaxf(CC_DAC_MIN, t->dac - CC_DAC_SLEW_V_S*dt);
  float hi = fminf(CC_DAC_MAX, t->dac + CC_DAC_SLEW_V_S*dt);
  float limited = clamp(raw, lo, hi);
  /* 将积分跟踪到实际受限输出，同时处理绝对饱和和斜率限制。
   * 仅冻结积分会使较大的 Ki 在斜率受限时完全不再移动。 */
  t->integral = t->gains.ki > 0.0f
      ? t->bias - t->gains.kp*e + t->gains.kd*t->derivative - limited : 0.0f;
  t->dac = limited;
  if (elapsed >= CC_VERIFY_STEP_MS) {
    t->step_peak = fmaxf(t->step_peak, current);
    float band = fmaxf(0.01f, 0.02f*t->setpoint);
    if (fabsf(e) > band) t->last_outside = elapsed - CC_VERIFY_STEP_MS;
    if (elapsed >= CC_VERIFY_END_MS - 1000U) {
      t->tail_sum += current; ++t->tail_count;
      t->tail_min = fminf(t->tail_min, current);
      t->tail_max = fmaxf(t->tail_max, current);
    }
  }
  if (elapsed >= CC_VERIFY_END_MS && t->tail_count != 0U) {
    t->overshoot_pct = fmaxf(0.0f, (t->step_peak - t->setpoint)/CC_VERIFY_STEP_A*100.0f);
    t->final_error = t->setpoint - t->tail_sum/(float)t->tail_count;
    t->ripple = t->tail_max - t->tail_min;
    t->settling_ms = (elapsed - CC_VERIFY_STEP_MS - t->last_outside >= 500U)
                     ? (float)t->last_outside : -1.0f;
    CC_Stop(t);
    t->verify_valid = true;
  }
}

void CC_Update(CCTuner *t, uint32_t now, CCMeasurement m)
{
  if (!CC_Active(t)) return;
  if (!measurement_ok(t, m)) return;
  uint32_t delta = now - t->previous_tick;
  t->previous_tick = now;
  if (t->output_on && (delta == 0U || delta > CC_MAX_GAP_MS)) {
    CC_Fail(t, CC_TIMING); return;
  }
  switch (t->state) {
    case CC_PRECHARGE:
      if ((uint32_t)(now - t->started) < CC_PRECHARGE_MS) break;
      if (m.vout > CC_PRECHARGE_V) {
        if ((uint32_t)(now - t->started) >= CC_PRECHARGE_MAX_MS) CC_Fail(t, CC_RANGE);
        break;
      }
      if (m.iout > 0.05f) { CC_Fail(t, CC_RANGE); break; }
      t->output_on = true;
      t->started = t->changed = now;
      if (t->verify_pending) {
        t->state = CC_VERIFY;
        t->previous_i = m.iout; t->derivative = 0.0f;
        t->integral = t->gains.ki > 0.0f
            ? t->bias - t->gains.kp*(t->target - m.iout) - CC_DAC_MAX : 0.0f;
        t->step_peak = t->tail_sum = 0.0f; t->tail_count = 0U;
        t->tail_min = 100.0f; t->tail_max = 0.0f; t->last_outside = 0U;
      } else t->state = CC_SEEK;
      break;
    case CC_SEEK:
      if ((uint32_t)(now - t->started) > CC_SEEK_TIMEOUT_MS) { CC_Fail(t, CC_TIMEOUT); break; }
      if ((uint32_t)(now - t->changed) < CC_SEEK_HOLD_MS) break;
      if (m.iout >= t->target) {
        if (t->dac < CC_DAC_MIN + CC_RELAY_V || t->dac > CC_DAC_MAX - CC_RELAY_V) {
          CC_Fail(t, CC_RANGE); break;
        }
        t->bias = t->dac; t->state = CC_RELAY;
        t->high = true; t->anchored = false;
        t->changed = t->high_start = now;
        t->peak = t->trough = m.iout;
        t->dac = t->bias - CC_RELAY_V;
      } else if (t->dac <= CC_DAC_MIN + CC_RELAY_V) CC_Fail(t, CC_RANGE);
      else {
        t->dac = fmaxf(CC_DAC_MIN + CC_RELAY_V, t->dac - CC_SEEK_STEP_V);
        t->changed = now;
      }
      break;
    case CC_RELAY: relay_step(t, now, m.iout); break;
    case CC_VERIFY: verify_step(t, now, m.iout, (float)delta*0.001f); break;
    default: break;
  }
  if (!isfinite(t->dac)) CC_Fail(t, CC_BAD_NUMBER);
}

const char *CC_ErrorText(CCError e)
{
  static const char *const names[] = {
    "OK", "ADC FAIL", "VIN LOW", "OVER CURRENT", "OVER VOLTAGE", "LOOP TOO SLOW",
    "OUT OF RANGE", "NO CROSS/TIMEOUT", "CYCLE TOO FAST", "NOISY/UNSTABLE",
    "RELAY ASYMMETRY", "NAN/INF"
  };
  return (unsigned)e < sizeof(names)/sizeof(names[0]) ? names[e] : "UNKNOWN";
}
