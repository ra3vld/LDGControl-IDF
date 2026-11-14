#!/usr/bin/env python3
"""Prepare the pinned ESP-IDF in a project-local directory and build; never flash."""
from pathlib import Path
import argparse, json, os, subprocess, sys
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--board',choices=['headless','sunton','guition'],default='sunton');p.add_argument('--sdk-dir',type=Path);p.add_argument('--version',default='0.1.0');a=p.parse_args()
sdk=(a.sdk_dir or ROOT/'.tools/esp-idf').resolve();tools=ROOT/'.tools/idf-tools'
env=dict(os.environ,IDF_TOOLS_PATH=str(tools),IDF_PATH=str(sdk))
if os.name=='nt' and len(str(sdk))>75:raise SystemExit('Choose a short checkout or --sdk-dir C:\\idf-5.5.5; Windows SDK extraction needs short paths. No system settings are changed.')
if not sdk.exists():
    sdk.parent.mkdir(parents=True,exist_ok=True)
    subprocess.run(['git','-c','core.longpaths=true','clone','--branch','v5.5.5','--depth','1','--recursive','--shallow-submodules','https://github.com/espressif/esp-idf.git',str(sdk)],check=True)
version=(sdk/'tools/cmake/version.cmake').read_text()
if 'IDF_VERSION_PATCH 5' not in version or 'IDF_VERSION_MINOR 5' not in version or 'IDF_VERSION_MAJOR 5' not in version:raise SystemExit('This project requires ESP-IDF 5.5.5')
runner=[sys.executable,str(sdk/'tools/idf_tools.py')]
subprocess.run(runner+['install','--targets=esp32,esp32s3'],env=env,check=True)
subprocess.run(runner+['install-python-env'],env=env,check=True)
export=subprocess.check_output(runner+['export','--format=key-value'],env=env,text=True)
for line in export.splitlines():
    if '=' not in line:continue
    key,value=line.split('=',1)
    if key not in ('PATH','IDF_PYTHON_ENV_PATH','OPENOCD_SCRIPTS','ESP_ROM_ELF_DIR','IDF_VERSION'):continue
    env[key]=value.replace('$PATH',env.get('PATH','')).replace('%PATH%',env.get('PATH',''))
python=Path(env['IDF_PYTHON_ENV_PATH'])/('Scripts/python.exe' if os.name=='nt' else 'bin/python')
subprocess.run([str(python),'-m','pip','install','-r',str(ROOT/'tools/idf-requirements.txt')],env=env,check=True)
subprocess.run([str(python),str(ROOT/'tools/build.py'),'--board',a.board,'--version',a.version],cwd=ROOT,env=env,check=True)
