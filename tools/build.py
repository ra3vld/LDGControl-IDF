#!/usr/bin/env python3
"""Build one board with the activated ESP-IDF 5.5.5 environment; never flash."""
from pathlib import Path
import argparse, os, subprocess, sys, importlib.metadata
p=argparse.ArgumentParser();p.add_argument('--board',choices=['headless','sunton','guition'],default='sunton');p.add_argument('--idf-path',default=os.environ.get('IDF_PATH'));p.add_argument('--version',default='0.1.0');p.add_argument('--configure-only',action='store_true');args=p.parse_args()
root=Path(__file__).resolve().parents[1]
if not args.idf_path:raise SystemExit('Activate ESP-IDF 5.5.5 (export.sh/export.bat), or pass --idf-path.')
if importlib.metadata.version('idf-component-manager')!='2.4.2':raise SystemExit('Install pinned IDF tools in the active SDK environment: python -m pip install -r tools/idf-requirements.txt')
env=dict(os.environ,IDF_PATH=str(Path(args.idf_path).resolve()))
# A Git commit alone does not trigger CMake. Refresh its generated identity
# header so an incremental build can never keep the previous commit marker.
if (root/('build-'+args.board)/'CMakeCache.txt').exists():
    subprocess.run([sys.executable,str(root/'tools/firmware_identity.py'),str(root/('build-'+args.board)/'generated/build_identity.hpp')],check=True)
command=[sys.executable,str(Path(args.idf_path)/'tools/idf.py'),'-B',str(root/('build-'+args.board)),'-D','LDG_BOARD='+args.board,'-D','PROJECT_VER='+args.version,'reconfigure' if args.configure_only else 'build']
subprocess.run(command,cwd=root,env=env,check=True)
subprocess.run([sys.executable,str(root/'tools/verify_components.py'),'--board',args.board],cwd=root,env=env,check=True)
if not args.configure_only:
    binary=root/('build-'+args.board)/'ldgcontrol.bin'
    limit=0x190000 if args.board!='guition' else 0x600000
    if binary.stat().st_size>limit:raise SystemExit(f'Image budget exceeded: {binary.stat().st_size} > {limit}')
    print(f'{args.board}: {binary.stat().st_size} bytes; budget {limit}; not flashed')
