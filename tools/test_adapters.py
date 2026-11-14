#!/usr/bin/env python3
"""Exercise production persistence with host NVS transaction/failure adapters."""
from pathlib import Path
import subprocess,os
root=Path(__file__).resolve().parents[1];out=root/'build-host';out.mkdir(exist_ok=True)
for board in ['sunton','headless','guition']:
    binary=out/('settings-'+board)
    cmd=['g++','-std=c++20','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-DLDG_BOARD_'+board+'=1',
     '-Itests/adapters/stubs','-Imain','-Icomponents/ldg_core/include','-isystem','components/arduinojson/include','tests/adapters/settings.cpp','-o',str(binary)]
    if os.environ.get('SANITIZE')=='1':cmd[1:1]=['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
    subprocess.run(cmd,cwd=root,check=True);subprocess.run([str(binary)],check=True)

for adapter in ['usb','wifi_storage']:
    cmd=['g++','-std=c++20','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-DLDG_BOARD_sunton=1','-DLDG_BOARD_NAME="sunton"','-DLDG_FIRMWARE_VERSION="0.1.0"',
     '-Itests/adapters/stubs','-Imain','-Icomponents/ldg_core/include','-isystem','components/arduinojson/include','tests/adapters/'+adapter+'.cpp',
     *map(str,(root/'components/ldg_core/src').glob('*.cpp')),'-o',str(out/adapter)]
    if os.environ.get('SANITIZE')=='1':cmd[1:1]=['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
    subprocess.run(cmd,cwd=root,check=True);subprocess.run([str(out/adapter)],check=True)
