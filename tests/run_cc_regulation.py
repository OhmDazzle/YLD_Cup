#!/usr/bin/env python3
"""Exercise the production CC loop, HAL sampling, protections and quantized DAC."""
from run_main_logic import run

run(r'''
static void reset_cc(float initial_dac, float current) {
    stage=0; codes[4]=2836; codes[3]=1418; codes[15]=0;
    App_SetState(APP_ST_STANDBY);
    targetI=1.0f;
    App_SetState(APP_ST_CC);
    assert(appState==APP_ST_CC && (ga.out & GPIO_PIN_1));
    near(dacAppliedV,DAC_OUTPUT_MAX_V,0.001f);
    ccDacV=initial_dac; App_DacSetVoltage(ccDacV);
    measVin=16; measVout=8; measIout=current;
}

static void feedback(float current, uint32_t dt, int fresh_voltage) {
    tick+=dt; measIout=current;
    currentSampleTick=tick; ccCurrentFresh=1;
    if (fresh_voltage) voltageSampleTick=tick;
    App_ControlStep();
}

static void prime(float current) {
    reset_cc(2.0f,current);
    feedback(current,10,1);
    assert(ccFilterReady);
    near(ccDacV,2.0f,0.000001f);
}

static uint32_t current_code(float current) {
    uint32_t lo=0, hi=4095;
    while (lo<hi) {
        uint32_t mid=(lo+hi)/2;
        if (Cal_Iout(mid*ADC_VREF/ADC_FULL_SCALE)<current) lo=mid+1;
        else hi=mid;
    }
    if (lo>0 && fabsf(Cal_Iout((lo-1)*ADC_VREF/ADC_FULL_SCALE)-current)<
                  fabsf(Cal_Iout(lo*ADC_VREF/ADC_FULL_SCALE)-current)) --lo;
    return lo;
}

static uint32_t voltage_code(float output, float current) {
    float nominal=(output-current*(CAL_VOUT_CURRENT_V_PER_A+CAL_VOUT_TRIM_I)
                   -CAL_VOUT_OFFSET_V-CAL_VOUT_TRIM_B)/CAL_VOUT_K;
    return (uint32_t)(fmaxf(0,nominal)/7*ADC_FULL_SCALE/ADC_VREF+0.5f);
}

static float plant_equilibrium(float resistance) {
    float command=CAL_DAC_BASE_A-CAL_DAC_BASE_B*dacAppliedV;
    return fmaxf(0,(CAL_DAC_K_0*command+CAL_DAC_B_0)/
                   (1+(0.003f*command+0.1042f)/resistance));
}

static void plant_case(float resistance, float setpoint, float tau, int delayed) {
    /* Hypothetical resistive first-order plants, NOT measured board dynamics.
     * Actual scheduling, calibration, ADC/DAC quantization and OCP/UVP run here.
     * Include long OLED-like loop jitter and an optional 30ms transport delay. */
    tick=0; reset_cc(DAC_OUTPUT_MAX_V,0);
    targetI=setpoint;
    float voltage=plant_equilibrium(resistance);
    float max_tail_error=0, acquisition=-1, peak=0, load_peak=0, load_jump=0;
    float delay_line[30]; unsigned cursor=0;
    for (unsigned n=0;n<30;++n) delay_line[n]=voltage;
    const uint32_t jitter[]={10,11,13,30,80,10};
    uint32_t elapsed=0, n=0;
    while (elapsed<90000) {
        uint32_t dt=jitter[n++%6];
        tick+=dt; elapsed+=dt;
        float actual_current=voltage/resistance;
        codes[15]=current_code(actual_current); codes[12]=codes[15];
        float measured_current=Cal_Iout(codes[15]*ADC_VREF/ADC_FULL_SCALE);
        codes[3]=voltage_code(voltage,measured_current); codes[4]=2836;
        App_SampleScheduler(); App_ProtectionCheck(); App_ControlStep();
        if(appState!=APP_ST_CC) fprintf(stderr,"CC plant fault state=%d t=%u R=%f I=%f target=%f\n",
                                       appState,elapsed,resistance,measIout,targetI);
        assert(appState==APP_ST_CC && (ga.out & GPIO_PIN_1));
        assert(dacAppliedV>=App_TargetToDac(CC_VOLTAGE_LIMIT_V)-0.001f);
        assert(dacAppliedV<=DAC_OUTPUT_MAX_V+0.001f);
        for (unsigned ms=0; ms<dt; ++ms) {
            float equilibrium=plant_equilibrium(resistance);
            float drive=delayed ? delay_line[cursor] : equilibrium;
            delay_line[cursor]=equilibrium; cursor=(cursor+1)%30;
            voltage+=0.001f/(tau+0.001f)*(drive-voltage);
        }
        float error=fabsf(voltage/resistance-setpoint);
        if (acquisition<0 && error<CC_FINE_ENTER_A) acquisition=elapsed*0.001f;
        if (elapsed<50000) peak=fmaxf(peak,voltage/resistance-setpoint);
        else load_peak=fmaxf(load_peak,voltage/resistance-setpoint);
        if (elapsed>75000) max_tail_error=fmaxf(max_tail_error,error);
        /* A setpoint step, followed by a load change, requires reacquisition. */
        if (elapsed>=30000 && elapsed-dt<30000) {
            App_HandleKeyEvent(KEY_IDX_UP,KEY_EV_SHORT); setpoint=targetI;
        }
        if (elapsed>=40000 && elapsed-dt<40000) {
            App_HandleKeyEvent(KEY_IDX_DOWN,KEY_EV_SHORT); setpoint=targetI;
        }
        if (elapsed>=50000 && elapsed-dt<50000) {
            resistance*=0.90f;
            load_jump=voltage/resistance-setpoint;
        }
    }
    printf("Synthetic CC R=%.1fohm target=%.2fA tau=%.2fs delay=%dms: "
           "acquire=%.2fs tail=%.2fmA startup/step overshoot=%.2fmA load peak=%.2fmA\n",
           resistance,setpoint,tau,delayed?30:0,acquisition,
           max_tail_error*1000,peak*1000,load_peak*1000);
    assert(acquisition>=0 && acquisition<15.0f);
    assert(max_tail_error<0.020f && peak<0.100f);
    /* A 10% resistance drop causes an immediate physical current jump. */
    assert(load_peak<fmaxf(0,load_jump)+0.010f);
    assert(ccFineControl);
}

static void compliance_case(void) {
    tick=0; reset_cc(DAC_OUTPUT_MAX_V,0); targetI=1;
    float resistance=40, voltage=plant_equilibrium(resistance);
    for (unsigned n=0;n<9000;++n) {
        tick+=10;
        codes[15]=current_code(voltage/resistance);
        codes[3]=voltage_code(voltage,Cal_Iout(codes[15]*ADC_VREF/ADC_FULL_SCALE));
        App_SampleScheduler(); App_ProtectionCheck(); App_ControlStep();
        assert(appState==APP_ST_CC);
        voltage+=0.01f/0.11f*(plant_equilibrium(resistance)-voltage);
        if (n==3000) {
            assert(ccLimited && voltage<CC_VOLTAGE_LIMIT_V+0.1f);
            assert(voltage>CC_VOLTAGE_LIMIT_V-0.1f && voltage/resistance<0.4f);
            resistance=8; /* Reachable load returns after 30s at voltage limit. */
        }
    }
    assert(fabsf(voltage/resistance-targetI)<0.015f && !ccLimited);
}

int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    /* Negative plant polarity, separate fresh flag, elapsed time, rate limits. */
    tick=100; prime(0.5f); feedback(0.5f,10,1);
    assert(ccDacV<2.0f && 2-ccDacV<=CC_FAST_RATE_V_S*0.01f+0.000001f);
    float saved=ccDacV;
    for (unsigned n=0;n<1000;++n) { tick+=10; App_ControlStep(); }
    near(ccDacV,saved,0.000001f);
    prime(1.5f); feedback(1.5f,10,1); assert(ccDacV>2.0f);
    prime(0.5f); feedback(0.5f,100,1); float change=2-ccDacV;
    prime(0.5f); for(unsigned n=0;n<10;++n) feedback(0.5f,10,1);
    near(2-ccDacV,change,0.000002f);

    /* Fast/fine hysteresis and small deviations of either sign. */
    prime(0.98f); feedback(0.98f,10,1); assert(ccFineControl && ccDacV<2);
    float fine_change=2-ccDacV;
    prime(0.5f); feedback(0.5f,10,1); assert(2-ccDacV>fine_change*5);
    prime(0.98f); feedback(0.98f,10,1);
    ccFilteredIout=0.95f; feedback(0.95f,10,1); assert(ccFineControl);
    feedback(0.90f,10,1); assert(!ccFineControl);
    for (unsigned sign=0;sign<2;++sign) {
        float value=sign ? 1.004f : 0.996f;
        prime(value);
        for(unsigned n=0;n<100;++n) feedback(value,10,1);
        near(ccDacV,2,0.000001f);
    }
    prime(1);
    for(unsigned n=0;n<1000;++n) feedback(n%2?1.006f:0.994f,10,1);
    near(ccDacV,2,0.000001f);
    prime(0.9f); feedback(1.01f,10,1); near(ccDacV,2,0.000001f);
    prime(1.1f); feedback(0.99f,10,1); near(ccDacV,2,0.000001f);

    /* Long gaps reseed, stale voltage/low Vin block only output increases. */
    prime(0.5f); saved=ccDacV; feedback(0.5f,5000,1);
    near(ccDacV,saved,0.000001f);
    feedback(0.5f,10,1); assert(ccDacV<saved);
    prime(0.5f); feedback(0.5f,250,0); saved=ccDacV;
    feedback(0.5f,10,0); near(ccDacV,saved,0.000001f);
    prime(0.5f); measVin=12; feedback(0.5f,10,1); near(ccDacV,2,0.000001f);
    ccFilteredIout=1.5f; feedback(1.5f,10,1); assert(ccDacV>2);

    /* No hidden integral at saturation: reversal works on the next sample. */
    prime(0);
    for(unsigned n=0;n<1000;++n) feedback(0,10,1);
    near(ccDacV,App_TargetToDac(CC_VOLTAGE_LIMIT_V),0.000001f); assert(ccLimited);
    saved=ccDacV; feedback(1.5f,100,1); assert(ccDacV>saved);
    prime(1.5f);
    for(unsigned n=0;n<1000;++n) feedback(1.5f,10,1);
    near(ccDacV,DAC_OUTPUT_MAX_V,0.000001f); assert(ccLimited);
    saved=ccDacV; feedback(0.5f,100,1); assert(ccDacV<saved);
    prime(0.5f); measVout=CC_VOLTAGE_LIMIT_V;
    feedback(0.5f,10,1); near(ccDacV,2,0.000001f); assert(ccLimited);
    measVout=CC_VOLTAGE_LIMIT_V+0.2f; feedback(0.5f,10,1); assert(ccDacV>2);

    /* CV -> CC tracks the real DAC; changed current target keeps its command. */
    codes[4]=2836; codes[15]=current_code(1); App_SetState(APP_ST_CV);
    App_DacSetVoltage(2.1f); saved=dacAppliedV;
    App_HandleKeyEvent(KEY_IDX_FUNC,KEY_EV_LONG);
    near(ccDacV,saved,0.000001f); assert(!ccCurrentFresh && !ccFineControl);
    targetI=1; ccFineControl=1;
    App_HandleKeyEvent(KEY_IDX_UP,KEY_EV_SHORT);
    near(targetI,1.1f,0.000001f); near(ccDacV,saved,0.000001f); assert(!ccFineControl);
    targetI=IOUT_TARGET_MAX; ccFineControl=1;
    App_HandleKeyEvent(KEY_IDX_UP,KEY_EV_SHORT); assert(ccFineControl);
    targetI=IOUT_TARGET_MIN;
    App_HandleKeyEvent(KEY_IDX_DOWN,KEY_EV_SHORT); near(targetI,IOUT_TARGET_MIN,0.000001f);
    App_SetState(APP_ST_PROT_UVP); assert(!ccFilterReady && !ccCurrentFresh);
    App_SetState(APP_ST_CC); near(ccDacV,DAC_OUTPUT_MAX_V,0.000001f);
    App_HandleKeyEvent(KEY_IDX_FUNC,KEY_EV_LONG);
    assert(appState==APP_ST_CV && !ccFineControl && !ccFilterReady);
    near(dacAppliedV,App_TargetToDac(targetV),0.001f);

    /* Protection's sample consumption cannot consume the controller's sample. */
    prime(0.5f); tick+=10; codes[15]=current_code(0.5f);
    assert(App_SampleCurrentGroup()); assert(ioutFresh && ccCurrentFresh);
    protBlankUntil=tick; App_ProtectionCheck(); assert(!ioutFresh && ccCurrentFresh);
    voltageSampleTick=tick; saved=ccDacV; App_ControlStep(); assert(ccDacV<saved);
    assert(!ccCurrentFresh);
    prime(1); tick=protBlankUntil; measIout=2.4f; ioutFresh=1; App_ProtectionCheck();
    tick+=OCP_CONFIRM_MS; currentSampleTick=tick; ccCurrentFresh=ioutFresh=1;
    App_ProtectionCheck(); uint32_t saved_code=dacCode; App_ControlStep();
    assert(appState==APP_ST_PROT_OCP && !(ga.out & GPIO_PIN_1));
    assert(!ccCurrentFresh && !ccFilterReady && dacCode==saved_code);
    prime(1); stage=4; assert(!App_SampleCurrentGroup());
    assert(appState==APP_ST_PROT_ADC && !ccCurrentFresh && !(ga.out & GPIO_PIN_1));
    prime(1); measIout=NAN; App_ControlStep(); assert(appState==APP_ST_PROT_ADC);
    prime(1); targetI=INFINITY; App_ControlStep(); assert(appState==APP_ST_PROT_ADC);
    tick=UINT32_MAX-15; prime(0.5f); feedback(0.5f,10,1); assert(ccDacV<2);

    plant_case(4,2.0f,0.02f,0);
    plant_case(8,1.0f,0.15f,1);
    plant_case(12,1.0f,0.30f,1);
    plant_case(20,0.2f,0.05f,0);
    compliance_case();
    puts("PASS: CC fast/fine regulation, deadband, jitter/wrap, saturation, transitions and protections");
}
''')
