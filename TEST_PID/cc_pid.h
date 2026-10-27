#ifndef CC_PID_H
#define CC_PID_H
#include <stdbool.h>
#include <stdint.h>
#include "cc_config.h"

typedef struct { float kp, ki, kd; } CCGains;
typedef struct { float vin, vout, iin, iout; bool valid; } CCMeasurement;
typedef enum {
  CC_IDLE, CC_PRECHARGE, CC_SEEK, CC_RELAY, CC_READY, CC_VERIFY, CC_FAULT
} CCState;
typedef enum {
  CC_OK, CC_ADC, CC_UVP, CC_OCP, CC_OVP, CC_TIMING, CC_RANGE,
  CC_TIMEOUT, CC_TOO_FAST, CC_NOISY, CC_ASYMMETRY, CC_BAD_NUMBER
} CCError;

typedef struct {
  CCState state;
  CCError error;
  bool output_on, valid_result, use_pid, verify_pending, high, anchored;
  bool verify_valid, manual_result;
  float target, setpoint, dac, bias, ku, tu, amplitude, scale;
  CCGains pi, pid, gains;
  float integral, previous_i, derivative;
  float peak, trough, periods[CC_CYCLES], amplitudes[CC_CYCLES];
  unsigned cycles, accepted;
  uint32_t started, previous_tick, changed, cycle_start, high_start;
  uint32_t cycle_samples, last_cycle_ms, last_cycle_samples;
  float last_cycle_amplitude;
  float overshoot_pct, final_error, ripple, settling_ms;
  float step_peak, tail_sum, tail_min, tail_max;
  unsigned tail_count;
  uint32_t last_outside;
} CCTuner;

void CC_Init(CCTuner *t, float target);
bool CC_Start(CCTuner *t, uint32_t now, bool verify);
void CC_Stop(CCTuner *t);
void CC_Fail(CCTuner *t, CCError error);
void CC_SelectGains(CCTuner *t, bool pid, float scale);
bool CC_LoadManual(CCTuner *t, CCGains gains, float bias);
void CC_Update(CCTuner *t, uint32_t now, CCMeasurement m);
bool CC_Active(const CCTuner *t);
const char *CC_ErrorText(CCError e);

#endif
