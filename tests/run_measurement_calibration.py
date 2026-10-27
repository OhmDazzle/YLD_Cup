#!/usr/bin/env python3
"""Replay historical current measurements; voltage V2 is tested separately."""
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
data = json.loads((ROOT / "calibration/measurements_2026-10-25.json").read_text())
cases = []
for row in data["rows"]:
    cases.append("{%s,%s,%s,%s}" % (row[10], row[11], row[3], row[6]))

source = r'''
#include "measurement_calibration.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
typedef struct { float raw_in, raw_out, ref_in, ref_out; } Row;
static const Row rows[] = { ROWS };
int main(void) {
    float ein=0, eout=0;
    for (unsigned n=0; n<sizeof(rows)/sizeof(rows[0]); n++) {
        Row r=rows[n];
        float iin=Cal_Iin(r.raw_in), iout=Cal_Iout(r.raw_out);
        float a=fabsf(iin-r.ref_in), b=fabsf(iout-r.ref_out);
        assert(a < 0.014f && b < 0.010f);
        ein+=a; eout+=b;
    }
    assert(ein/35 < 0.003f && eout/35 < 0.0032f);
    /* True zero and out-of-range current must not be turned into a positive
       zero-offset reading or a clipped current below the OCP threshold. */
    assert(Cal_Iin(0)==0 && Cal_Iout(0)==0 && Cal_Vout(0,0)==0);
    assert(Cal_Iin(-0.1f)==0 && Cal_Iout(-0.1f)==0);
    float old_in=0, old_out=0;
    for(unsigned code=0; code<=4095; code++) {
        float pin=code*3.3f/4095;
        float a=Cal_Iin(pin), b=Cal_Iout(pin);
        assert(isfinite(a) && isfinite(b) && a>=old_in && b>=old_out);
        old_in=a; old_out=b;
    }
    assert(Cal_Iout(2.3f)<2.3f && Cal_Iout(2.5f)>2.3f);
    assert(Cal_Iout(3.3f)>3.0f && Cal_Iin(3.3f)>3.0f);
    assert(fabsf(Cal_Iout(2.07001f)-Cal_Iout(2.06999f))<0.0001f);
    assert(fabsf(Cal_Iin(1.27001f)-Cal_Iin(1.26999f))<0.0001f);
    printf("PASS: 35 current pairs; MAE Iin=%.3fmA Iout=%.3fmA; monotonic ADC range and OCP headroom\n",
           ein/35*1000, eout/35*1000);
}
'''.replace("ROWS", ",\n".join(cases))

with tempfile.TemporaryDirectory(prefix="yld-measurement-test-") as directory:
    path = Path(directory)
    (path / "cases.c").write_text(source)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "Core/Inc"),
                    str(path / "cases.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
