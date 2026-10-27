#!/usr/bin/env python3
"""Host checks for CC identification, verification and shutdown; no hardware claim."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
CASES = r'''
#include "cc_pid.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static CCMeasurement good(float i) {
  return (CCMeasurement){16.0f, 8.0f, i*0.6f, i, true};
}
static void near(float a, float b, float eps) { assert(fabsf(a-b) < eps); }

/* Artificial 8ohm plants with configurable delay and time constant.
 * Includes a faster relay period previously rejected by the 10ms firmware. */
static void simulated_run(uint32_t initial_tick, unsigned delay_ms, float tau_ms) {
  CCTuner t;
  CC_Init(&t, 1.0f);
  uint32_t now = initial_tick;
  assert(CC_Start(&t, now, false));
  unsigned delay_size = delay_ms/CC_PERIOD_MS;
  assert(delay_size > 0U);
  float delay[delay_size], i = 0.0f;
  for(unsigned j=0U;j<delay_size;j++) delay[j]=0.0f;
  float alpha = (float)CC_PERIOD_MS/(tau_ms + (float)CC_PERIOD_MS);
  unsigned pos = 0U;
  for (unsigned n=0U; n<80000U/CC_PERIOD_MS && CC_Active(&t); ++n) {
    float command = t.output_on ? fmaxf(0.0f, (CC_DAC_A-CC_DAC_B*t.dac)/8.0f) : 0.0f;
    float delayed = delay[pos]; delay[pos] = command; pos=(pos+1U)%delay_size;
    i += (delayed-i)*alpha;
    CCMeasurement m=good(i);
    m.vout = CC_DAC_A-CC_DAC_B*t.dac;
    now+=CC_PERIOD_MS; CC_Update(&t,now,m);
    assert(t.dac>=CC_DAC_MIN-0.0001f && t.dac<=CC_DAC_MAX+0.0001f);
  }
  if(t.state != CC_READY) printf("simulation failed: %s cycles=%u amp=%f\n",CC_ErrorText(t.error),t.cycles,t.amplitude);
  assert(t.state==CC_READY && t.valid_result && !t.output_on);
  assert(t.accepted==CC_CYCLES && t.tu>=CC_MIN_CYCLE_MS*0.001f && t.ku>0.0f);
  assert(t.last_cycle_samples>=CC_MIN_CYCLE_SAMPLES);
  if(delay_ms<=10U) assert(t.tu<0.12f);
  near(t.pi.ki, 0.54f*t.ku/t.tu, 0.0001f);
  near(t.pid.kd, 0.075f*t.ku*t.tu, 0.0001f);
  near(t.gains.kp,t.pi.kp*0.25f,0.0001f);
  printf("synthetic plant: Ku=%.3f Tu=%.3fs PI=(%.3f,%.3f,%.3f)\n",t.ku,t.tu,t.gains.kp,t.gains.ki,t.gains.kd);

  assert(CC_Start(&t,now,true));
  i=0.0f; for(unsigned j=0;j<delay_size;j++) delay[j]=0.0f;
  for (unsigned n=0U; n<7000U/CC_PERIOD_MS && CC_Active(&t); ++n) {
    float old_dac=t.dac; bool verifying=t.state==CC_VERIFY;
    float command=t.output_on ? fmaxf(0.0f,(CC_DAC_A-CC_DAC_B*t.dac)/8.0f) : 0.0f;
    float delayed=delay[pos]; delay[pos]=command; pos=(pos+1U)%delay_size;
    i+=(delayed-i)*alpha;
    CCMeasurement m=good(i); m.vout=CC_DAC_A-CC_DAC_B*t.dac;
    now+=CC_PERIOD_MS; CC_Update(&t,now,m);
    if(verifying && t.output_on)
      assert(fabsf(t.dac-old_dac)<=CC_DAC_SLEW_V_S*CC_PERIOD_MS*0.001f+0.00001f);
  }
  assert(t.state==CC_READY && t.verify_valid && !t.output_on);
  assert(isfinite(t.final_error) && isfinite(t.overshoot_pct));
  assert(fabsf(t.final_error)<0.1f);
  printf("synthetic verification: error=%.4fA overshoot=%.1f%% ripple=%.4fA\n",t.final_error,t.overshoot_pct,t.ripple);
}

static CCTuner running(CCState state) {
  CCTuner t; CC_Init(&t,1.0f); t.state=state;
  t.output_on=true; t.dac=t.bias=2.0f; t.previous_tick=1000U;
  t.started=t.changed=t.cycle_start=t.high_start=1000U;
  return t;
}

int main(void) {
  simulated_run(0U,100U,50.0f);
  simulated_run(UINT32_MAX-15000U,100U,50.0f);
  simulated_run(0U,10U,5.0f);
  CCTuner t; CCMeasurement m;
  CC_Init(&t,NAN); near(t.target,1.0f,0.001f);
  assert(!CC_Start(&t,0U,true)); /* No coefficients yet. */
  assert(CC_Start(&t,0U,false));
  CC_Update(&t,399U,good(0.0f)); assert(!t.output_on);
  m=good(0.0f); m.vout=3.0f;
  CC_Update(&t,400U,m); assert(t.output_on && t.state==CC_SEEK);
  CC_Stop(&t); assert(!t.output_on && t.state==CC_IDLE);

  /* Manual fallback permits measured step testing even when relay is too fast. */
  assert(!CC_LoadManual(&t,(CCGains){NAN,1.0f,0.0f},2.0f));
  assert(!CC_LoadManual(&t,(CCGains){0.1f,-1.0f,0.0f},2.0f));
  assert(!CC_LoadManual(&t,(CCGains){0.1f,1.0f,0.0f},NAN));
  assert(CC_LoadManual(&t,(CCGains){0.1f,1.0f,0.0f},2.0f));
  assert(t.manual_result && t.valid_result && t.state==CC_READY);
  assert(CC_Start(&t,500U,true));
  assert(!CC_LoadManual(&t,(CCGains){0.2f,1.0f,0.0f},2.0f));

  CC_Init(&t,1.0f); CC_Start(&t,0U,false);
  CC_Update(&t,3000U,good(0.0f)); /* Stored capacitor voltage has not fallen. */
  assert(!t.output_on && t.error==CC_RANGE);

  /* Every active state responds to invalid ADC, NaN and protection limits. */
  CCState states[]={CC_PRECHARGE,CC_SEEK,CC_RELAY,CC_VERIFY};
  for(unsigned s=0;s<4;s++) {
    for(unsigned fault=0;fault<6;fault++) {
      t=running(states[s]); m=good(1.0f);
      CCError expected=CC_OK;
      if(fault==0) {m.valid=false; expected=CC_ADC;}
      if(fault==1) {m.iout=NAN; expected=CC_BAD_NUMBER;}
      if(fault==2) {m.vin=12.0f; expected=CC_UVP;}
      if(fault==3) {m.iout=1.4f; expected=CC_OCP;}
      if(fault==4) {m.vout=12.1f; expected=CC_OVP;}
      if(fault==5) expected=CC_TIMING;
      CC_Update(&t,fault==5?1030U:1010U,m);
      assert(t.state==CC_FAULT && t.error==expected && !t.output_on);
      near(t.dac,CC_DAC_MAX,0.0001f);
      assert(!CC_Start(&t,1040U,false)); /* Latched; no automatic restart. */
    }
  }
  /* No crossing, no headroom and under-resolved cycles must not yield gains. */
  t=running(CC_RELAY); t.previous_tick=6008U; CC_Update(&t,6010U,good(1.0f));
  assert(t.error==CC_TIMEOUT);
  t=running(CC_SEEK); t.previous_tick=1108U; t.dac=CC_DAC_MAX;
  CC_Update(&t,1110U,good(1.0f)); assert(t.error==CC_RANGE);
  t=running(CC_RELAY); t.anchored=t.high=true; t.cycles=CC_DISCARD_CYCLES;
  t.previous_tick=1098U; t.cycle_start=1080U; t.high_start=1090U;
  t.cycle_samples=9U; t.peak=1.04f; t.trough=0.96f;
  CC_Update(&t,1100U,good(1.01f)); assert(t.error==CC_TOO_FAST && !t.valid_result);
  assert(t.last_cycle_ms==20U && t.last_cycle_samples==10U);
  near(t.last_cycle_amplitude,0.04f,0.0001f);
  /* A long elapsed cycle with too few actual samples still fails. */
  t=running(CC_RELAY); t.anchored=t.high=true; t.cycles=CC_DISCARD_CYCLES;
  t.previous_tick=1298U; t.high_start=1150U;
  t.cycle_samples=6U; t.peak=1.04f; t.trough=0.96f;
  CC_Update(&t,1300U,good(1.01f)); assert(t.error==CC_TOO_FAST);
  assert(t.last_cycle_ms==300U && t.last_cycle_samples==7U);
  t=running(CC_RELAY); t.anchored=t.high=true; t.cycles=CC_DISCARD_CYCLES;
  t.previous_tick=1298U; t.high_start=1150U; t.cycle_samples=149U;
  t.peak=1.004f; t.trough=0.996f;
  CC_Update(&t,1300U,good(1.004f)); assert(t.error==CC_NOISY);
  t=running(CC_RELAY); t.anchored=t.high=true; t.cycles=CC_DISCARD_CYCLES;
  t.previous_tick=1298U; t.high_start=1290U; t.cycle_samples=149U;
  t.peak=1.04f; t.trough=0.96f;
  CC_Update(&t,1300U,good(1.01f)); assert(t.error==CC_ASYMMETRY);
  t=running(CC_RELAY); t.anchored=t.high=true; t.cycles=CC_DISCARD_CYCLES+CC_CYCLES-1;
  t.accepted=CC_CYCLES-1;
  for(unsigned n=0;n<CC_CYCLES-1;n++) { t.periods[n]=200.0f; t.amplitudes[n]=0.04f; }
  t.previous_tick=1298U; t.high_start=1150U; t.cycle_samples=149U;
  t.peak=1.04f; t.trough=0.96f;
  CC_Update(&t,1300U,good(1.01f)); assert(t.error==CC_NOISY && !t.valid_result);

  /* Negative DAC path, measured dt, derivative on measurement (no setpoint kick),
   * tracking anti-windup at both rate and absolute limits. */
  t=running(CC_VERIFY); t.gains=(CCGains){0.1f,10.0f,0.01f};
  t.integral=-0.05f; t.previous_i=0.5f; t.target=1.0f;
  CC_Update(&t,1002U,good(0.5f)); assert(t.dac<2.0f);
  near(t.derivative,0.0f,0.0001f);
  float first=t.dac; CC_Update(&t,1004U,good(0.5f)); assert(t.dac<first);
  t.dac=CC_DAC_MIN; t.integral=2.0f;
  for(unsigned n=0;n<20;n++) CC_Update(&t,1006U+2U*n,good(0.5f));
  near(t.dac,CC_DAC_MIN,0.0001f);
  CC_Update(&t,1046U,good(1.2f)); assert(t.dac>CC_DAC_MIN);
  CC_Stop(&t); assert(!t.verify_valid && !t.output_on);
  /* Ki=0 really disables integral action, including tracking offsets. */
  t=running(CC_VERIFY); t.gains=(CCGains){0.1f,0.0f,0.0f};
  t.previous_i=0.5f; t.integral=-0.05f;
  for(unsigned n=0;n<100;n++) CC_Update(&t,1002U+2U*n,good(0.5f));
  near(t.dac,1.95f,0.0001f); near(t.integral,0.0f,0.0001f);
  /* The faster loop accepts the configured maximum gap, rejects one ms more. */
  t=running(CC_VERIFY); t.gains=(CCGains){0.1f,1.0f,0.0f}; t.previous_i=1.0f;
  CC_Update(&t,1000U+CC_MAX_GAP_MS,good(1.0f));
  assert(t.output_on && t.error==CC_OK);
  CC_Update(&t,1001U+2U*CC_MAX_GAP_MS,good(1.0f));
  assert(!t.output_on && t.error==CC_TIMING);
  puts("PASS: CC identification, verification, timing, polarity, anti-windup and fault latches");
}
'''

with tempfile.TemporaryDirectory(prefix="yld-cc-test-") as directory:
    path = pathlib.Path(directory)
    (path / "cases.c").write_text(CASES)
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I", str(ROOT / "TEST_PID"), "-I", str(ROOT / "Core/Inc"), str(path / "cases.c"),
        str(ROOT / "TEST_PID/cc_pid.c"), "-lm", "-o", str(path / "test")
    ], check=True)
    subprocess.run([str(path / "test")], check=True)
