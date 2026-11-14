#!/usr/bin/env python3
"""Fingerprint firmware inputs for runtime identification and USB catalog checks."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
def git(*args):
    return subprocess.check_output(['git',*args],cwd=ROOT,text=True,stderr=subprocess.DEVNULL).strip()
def inputs():
    paths=[]
    for directory in ['main','components','boards','web/src']:
        paths += [p for p in (ROOT/directory).rglob('*') if p.is_file() and '__pycache__' not in p.parts]
    paths += [ROOT/p for p in ['CMakeLists.txt','dependencies.esp32.lock','dependencies.esp32s3.lock',
        'web/package.json','web/package-lock.json','tools/build.py','tools/build_web.py',
        'tools/web_tools.py','tools/firmware_identity.py','tools/idf-requirements.txt','release-public.pem']]
    return sorted(paths)
def identity():
    def content(p):
        data=p.read_bytes()
        # Component Manager emits trailing YAML whitespace. It does not change
        # the dependency contract; normalize it before hashing and packaging.
        return b'\n'.join(line.rstrip() for line in data.splitlines())+b'\n' if p.suffix=='.lock' else data
    manifest={p.relative_to(ROOT).as_posix():hashlib.sha256(content(p)).hexdigest() for p in inputs()}
    digest=hashlib.sha256(json.dumps(manifest,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    try: commit=git('rev-parse','HEAD')
    except subprocess.CalledProcessError: commit='unknown'
    return {'commit':commit,'source_digest':digest}
if __name__=='__main__':
    value=identity()
    content='#pragma once\n'+''.join(f'#define LDG_{key.upper() if key!="commit" else "BUILD_COMMIT"} {json.dumps(text)}\n' for key,text in value.items())
    path=Path(sys.argv[1]);path.parent.mkdir(parents=True,exist_ok=True)
    if not path.exists() or path.read_text()!=content:path.write_text(content)
