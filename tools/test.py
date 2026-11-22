#!/usr/bin/env python3
"""Compile and run host policies and integration regressions (no ESP32 required)."""
from pathlib import Path
import subprocess, os
root=Path(__file__).resolve().parents[1]
out=root/'build-host';out.mkdir(exist_ok=True)
includes=['-I'+str(root/p) for p in ['vendor/shack-libs/ldg_core/include','tests','tests/unity']]
includes+=['-isystem',str(root/'components/arduinojson/include')]
subprocess.run(['cc','-DUNITY_INCLUDE_DOUBLE',*includes,'-c',str(root/'tests/unity/unity.c'),'-o',str(out/'unity.o')],check=True)
for test in sorted((root/'tests').glob('*.cpp')):
    binary=out/test.stem
    cmd=['g++','-std=c++20','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-g','-O1','-DUNITY_INCLUDE_DOUBLE',*includes,str(test),str(out/'unity.o')]
    if not test.name.startswith('legacy_'):
        cmd += [str(p) for p in (root/'vendor/shack-libs/ldg_core/src').glob('*.cpp')]
    if os.environ.get('SANITIZE')=='1':cmd += ['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    subprocess.run(cmd+['-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
