#!/usr/bin/env python3
"""Replay both V2 PDF pages through the actual C voltage conversion functions."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "calibration"))
from fit_voltage_v2 import corrected_rows

data = json.loads((ROOT / "calibration/measurements_2026-10-27.json").read_text())
base = json.loads((ROOT / "calibration/fit_results.json").read_text())
kv, ki, b = base["vout"]["coefficients_voltage_current_offset"]
adc_rows = []
for r in corrected_rows(data, 1):
    current = float(r[11])  # already calibrated V2.06/V2.07 OLED current
    nominal = (float(r[9]) - ki * current - b) / kv
    adc_rows.append(f"{{{nominal:.10f}f,{current:.6f}f,{float(r[5]):.6f}f}}")
dac_rows = ["{%s,%s,%s}" % (r[1], r[4], r[3]) for r in corrected_rows(data, 2)]

source = r'''
#include "measurement_calibration.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
typedef struct { float nominal, current, reference; } AdcRow;
typedef struct { float command, current, measured; } DacRow;
static const AdcRow adc[] = { ADC_ROWS };
static const DacRow dac[] = { DAC_ROWS };
int main(void) {
    float sum_adc=0, max_adc=0, sum_dac=0, max_dac=0;
    for (unsigned n=0; n<20; ++n) {
        AdcRow a=adc[n];
        float e=fabsf(Cal_Vout(a.nominal,a.current)-a.reference);
        assert(e<0.0061f); sum_adc+=e; max_adc=fmaxf(max_adc,e);
        DacRow d=dac[n];
        float pin=Cal_TargetToDac(d.command,d.current);
        assert(pin>=0.2f && pin<=3.1f);
        unsigned code=(unsigned)(pin*4095.0f/3.3f+0.5f);
        float command=17.07091f-4.545455f*code*3.3f/4095.0f;
        /* Keep each measured anchor's residual when shifting its command. */
        float k=1.0115f-0.003f*d.current;
        e=fabsf(d.measured+k*(command-d.command)-d.command);
        assert(e<0.010f); sum_dac+=e; max_dac=fmaxf(max_dac,e);
    }
    assert(sum_adc/20<0.0021f && sum_dac/20<0.005f);
    /* Full sensing range stays monotonic and true zero stays zero. */
    for (unsigned i=0; i<=7; ++i) {
        float current=i*0.5f, old=0;
        assert(Cal_Vout(0,current)==0);
        for (unsigned code=0; code<=4095; ++code) {
            float v=Cal_Vout(code*3.3f/4095*7,current);
            assert(isfinite(v) && v>=old); old=v;
        }
    }
    for (unsigned i=0; i<=20; ++i) {
        float current=i*0.1f, old=10;
        for (unsigned v=500; v<=1500; ++v) {
            float d=Cal_TargetToDac(v*0.01f,current);
            assert(isfinite(d) && d<old); old=d;
        }
    }
    assert(Cal_TargetToDac(8,2)<Cal_TargetToDac(8,0));
    assert(Cal_TargetToDac(8,3)==Cal_TargetToDac(8,2));
    assert(Cal_TargetToDac(8,-1)==Cal_TargetToDac(8,0));
    for (unsigned n=0; n<2; ++n) {
        float v=n ? 9 : 5;
        assert(fabsf(Cal_TargetToDac(v-0.0001f,1)-Cal_TargetToDac(v+0.0001f,1))<0.0001f);
    }
    printf("PASS: ADC 20 pairs MAE=%.3fmV max=%.3fmV; DAC 20 anchors + quantization MAE=%.3fmV max=%.3fmV; range/continuity\n",
           sum_adc/20*1000,max_adc*1000,sum_dac/20*1000,max_dac*1000);
}
'''.replace("ADC_ROWS", ",\n".join(adc_rows)).replace("DAC_ROWS", ",\n".join(dac_rows))

with tempfile.TemporaryDirectory(prefix="yld-voltage-v2-") as directory:
    path = Path(directory)
    (path / "cases.c").write_text(source)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "Core/Inc"),
                    str(path / "cases.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
