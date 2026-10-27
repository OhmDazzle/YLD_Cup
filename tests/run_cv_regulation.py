#!/usr/bin/env python3
"""Exercise the actual main.c slow CV loop, including a synthetic thermal drift."""
from run_main_logic import run

run(r'''
static void reset_cv(void) {
    stage=0; targetV=8;
    codes[3]=1418; codes[4]=2836; codes[15]=0;
    App_SetState(APP_ST_CV);
    assert(appState==APP_ST_CV);
    measVin=16; measIout=2; measVout=8;
}

static void feedback(float voltage, uint32_t dt, int fresh_current) {
    tick+=dt;
    measVout=voltage;
    cvVoltageSampleTick=tick; cvVoltageFresh=1;
    if (fresh_current) cvCurrentSampleTick=tick;
    App_ControlStep();
}

static void prime(float voltage) {
    reset_cv();
    feedback(voltage,CV_TRIM_SETTLE_MS,1);
    assert(cvFilterReady && cvTrimDacV==0);
}

static uint32_t voltage_code(float terminal, float measured_current) {
    float nominal=(terminal - measured_current*(CAL_VOUT_CURRENT_V_PER_A+CAL_VOUT_TRIM_I)
                   - CAL_VOUT_OFFSET_V - CAL_VOUT_TRIM_B) / CAL_VOUT_K;
    float code=nominal/7.0f*4095.0f/3.3f;
    assert(code>=0 && code<4095);
    return (uint32_t)(code+0.5f);
}

static void thermal_model(void) {
    /* A hypothetical single-pole plant, not measured board dynamics. The old
     * measured affine transfer is perturbed by +60mV then -10mV/min for 20min.
     * Run the real scheduler, protection, ADC conversion and quantized DAC. */
    tick=0; reset_cv();
    uint32_t current_code=0;
    float current_error=10;
    for (uint32_t c=0; c<=4095; ++c) {
        float e=fabsf(Cal_Iout(c*3.3f/4095.0f)-2.0f);
        if (e<current_error) { current_error=e; current_code=c; }
    }
    float measured_current=Cal_Iout(current_code*3.3f/4095.0f);
    const uint32_t jitter[]={10,17,31,12,25,80};
    float output=8.06f, max_error=0, max_trim=0;
    uint32_t elapsed=0, n=0;
    while (elapsed<1200000U) {
        uint32_t dt=jitter[n++%6];
        elapsed+=dt; tick+=dt;
        codes[3]=voltage_code(output,measured_current);
        codes[4]=2836; codes[15]=current_code;
        App_SampleScheduler();
        App_ProtectionCheck();
        App_ControlStep();
        assert(appState==APP_ST_CV && (ga.out & GPIO_PIN_1));
        float bias=0.06f-0.01f*((float)elapsed/60000.0f);
        float pin=dacCode*3.3f/4095.0f;
        float equilibrium=1.0055f*(17.07091f-4.545455f*pin)-0.5209f+bias;
        float seconds=dt*0.001f;
        output+=seconds/(0.2f+seconds)*(equilibrium-output);
        assert(fabsf(cvTrimDacV)<=CV_TRIM_MAX_DAC_V+0.000001f);
        if (elapsed>60000U) max_error=fmaxf(max_error,fabsf(output-8));
        max_trim=fmaxf(max_trim,fabsf(cvTrimDacV));
    }
    assert(max_error<0.025f && fabsf(output-8)<0.020f);
    assert(cvTrimDacV<0 && max_trim<CV_TRIM_MAX_DAC_V);
    printf("Synthetic 8V/2A, 20min -10mV/min drift: final=%.4fV max error after 60s=%.2fmV trim=%.5fV\n",
           output,max_error*1000,cvTrimDacV);
}

int main(void) {
    /* Startup settling, correct negative-path polarity and fresh sample only. */
    tick=0; reset_cv();
    feedback(7.9f,CV_TRIM_SETTLE_MS-1,1);
    assert(!cvFilterReady && cvTrimDacV==0);
    feedback(7.9f,1,1); assert(cvFilterReady && cvTrimDacV==0);
    feedback(7.9f,100,1); assert(cvTrimDacV<0);
    float saved=cvTrimDacV;
    for (unsigned n=0; n<1000; ++n) { tick+=10; App_ControlStep(); }
    near(cvTrimDacV,saved,0.000001f);
    assert(!cvVoltageFresh);
    prime(8.1f); feedback(8.1f,100,1); assert(cvTrimDacV>0);
    prime(8.005f);
    for (unsigned n=0; n<50; ++n) feedback(8.005f,100,1);
    assert(cvTrimDacV==0);

    /* Equal real elapsed times yield the same correction despite loop jitter. */
    prime(7.9f);
    for (unsigned n=0; n<10; ++n) feedback(7.9f,100,1);
    saved=cvTrimDacV;
    prime(7.9f);
    for (unsigned n=0; n<5; ++n) feedback(7.9f,200,1);
    near(cvTrimDacV,saved,0.000001f);

    /* A long gap reseeds the filter instead of integrating a stale error. */
    saved=cvTrimDacV;
    feedback(7.5f,5000,1); near(cvTrimDacV,saved,0.000001f);
    feedback(7.5f,100,1);
    assert(saved-cvTrimDacV<=CV_TRIM_RATE_V_S*0.1f+0.000001f);

    /* Freeze for stale current, undervoltage, overcurrent or large collapse. */
    prime(7.5f); feedback(7.5f,600,0); feedback(7.5f,100,0);
    assert(cvTrimDacV==0);
    prime(7.5f); measVin=12; feedback(7.5f,100,1); assert(cvTrimDacV==0);
    prime(7.5f); measIout=2.4f; feedback(7.5f,100,1); assert(cvTrimDacV==0);
    prime(6.0f); feedback(6.0f,100,1); assert(cvTrimDacV==0);

    /* Total correction is bounded and reverses without hidden windup. */
    prime(7.5f);
    for (unsigned n=0; n<1000; ++n) feedback(7.5f,100,1);
    near(cvTrimDacV,-CV_TRIM_MAX_DAC_V,0.000001f); assert(cvTrimLimited);
    for (unsigned n=0; n<20; ++n) feedback(8.5f,100,1);
    assert(cvTrimDacV>-CV_TRIM_MAX_DAC_V+0.0001f && !cvTrimLimited);
    /* Physical output saturation also bounds the integrator itself. */
    targetV=100; measVout=99.5f; cvFilteredVout=99.5f;
    for (unsigned n=0; n<20; ++n) feedback(99.5f,100,1);
    assert(cvTrimDacV>=0 && cvTrimLimited);
    assert(dacCode==248);

    /* Target changes and state transitions never carry an old calibration. */
    prime(7.9f); cvTrimDacV=-0.05f;
    App_HandleKeyEvent(KEY_IDX_UP,KEY_EV_SHORT);
    assert(targetV==9 && cvTrimDacV==0 && !cvFilterReady);
    targetV=15; cvTrimDacV=-0.025f;
    App_HandleKeyEvent(KEY_IDX_UP,KEY_EV_SHORT);
    near(cvTrimDacV,-0.025f,0.000001f); /* No change at target limit. */
    App_SetState(APP_ST_CC); assert(cvTrimDacV==0);
    uint32_t saved_code=dacCode; App_ControlStep(); assert(dacCode==saved_code);
    cvTrimDacV=-0.05f; App_SetState(APP_ST_STANDBY); assert(cvTrimDacV==0);
    App_ControlStep(); assert(dacCode==saved_code);

    /* New voltage is independent of protection's freshness flags; current-only
     * resampling does not integrate the cached voltage a second time. */
    reset_cv(); cvVoltageFresh=0;
    assert(App_SampleCurrentGroup()); assert(!cvVoltageFresh);
    assert(App_SampleVoltageGroup()); assert(cvVoltageFresh && vinFresh);
    App_ProtectionCheck(); assert(!vinFresh && cvVoltageFresh);
    App_ControlStep(); assert(!cvVoltageFresh);
    cvTrimDacV=-0.05f; stage=4;
    assert(!App_SampleVoltageGroup());
    assert(appState==APP_ST_PROT_ADC && cvTrimDacV==0 && !(ga.out & GPIO_PIN_1));
    saved_code=dacCode; App_ControlStep(); assert(dacCode==saved_code);
    stage=0; reset_cv(); measVout=NAN; App_ControlStep();
    assert(appState==APP_ST_PROT_ADC && !(ga.out & GPIO_PIN_1));

    /* The actual protections run first and discard a pending trim update. */
    reset_cv(); cvTrimDacV=-0.05f; tick=protBlankUntil;
    measIout=2.4f; ioutFresh=1; App_ProtectionCheck();
    tick+=OCP_CONFIRM_MS; ioutFresh=1; cvVoltageFresh=1;
    App_ProtectionCheck(); saved_code=dacCode; App_ControlStep();
    assert(appState==APP_ST_PROT_OCP && cvTrimDacV==0 && dacCode==saved_code);

    tick=UINT32_MAX-200U; prime(7.9f); feedback(7.9f,100,1);
    assert(cvTrimDacV<0 && isfinite(cvTrimDacV));
    thermal_model();
    puts("PASS: CV polarity, freshness, time/wrap, deadband, rate/total/physical limits, anti-windup, resets and protection");
}
''')
