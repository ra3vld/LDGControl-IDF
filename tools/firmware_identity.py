#!/usr/bin/env python3
"""Fingerprint project sources and resolved dependencies for runtime/USB identity."""
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
def git(*args):
    # Containers can check out as one UID and compile as another. Trust only
    # this known project for read-only Git queries; never change global config.
    return subprocess.check_output(['git','-c','safe.directory='+str(ROOT),*args],cwd=ROOT,text=True,stderr=subprocess.PIPE).strip()
def inputs():
    paths=[]
    for directory in ['main','components','boards','web/src']:
        paths += [p for p in (ROOT/directory).rglob('*') if p.is_file() and '__pycache__' not in p.parts]
    paths += [ROOT/p for p in ['CMakeLists.txt','dependencies.esp32.lock','dependencies.esp32s3.lock',
        'web/package.json','web/package-lock.json','tools/build.py','tools/build_web.py',
        'tools/web_tools.py','tools/firmware_identity.py','tools/idf-requirements.txt',
        'tools/site-requirements.txt','release-public.pem']]
    return sorted(paths)
def resolved_lock(data):
    # manifest_hash depends on the configure pass and active SDK target. The
    # actual resolved code is bound by component versions, hashes and sources.
    # Descriptive registry rules/formatting do not identify different code.
    import yaml
    lock=yaml.safe_load(data)
    resolved={}
    for name,item in lock['dependencies'].items():
        source=dict(item['source'])
        if 'registry_url' in source:source['registry_url']=source['registry_url'].rstrip('/')
        value={'version':str(item['version']),'source':source}
        if source['type']!='idf':
            checksum=item['component_hash']
            if not re.fullmatch('[0-9a-f]{64}',checksum):raise ValueError('Invalid resolved component hash: '+name)
            value['component_hash']=checksum
        resolved[name]=value
    return {'format':lock['version'],'target':lock['target'],
            'direct_dependencies':sorted(lock['direct_dependencies']),'dependencies':resolved}
def manifest():
    return {p.relative_to(ROOT).as_posix():hashlib.sha256(
        json.dumps(resolved_lock(p.read_bytes()),sort_keys=True,separators=(',',':')).encode()
        if p.suffix=='.lock' else p.read_bytes()).hexdigest() for p in inputs()}
def identity():
    digest=hashlib.sha256(json.dumps(manifest(),sort_keys=True,separators=(',',':')).encode()).hexdigest()
    try: commit=git('rev-parse','HEAD')
    except (subprocess.CalledProcessError,FileNotFoundError) as error:
        if (ROOT/'.git').exists() or os.environ.get('GITHUB_ACTIONS')=='true':
            raise RuntimeError('Cannot identify the source checkout; refusing an unknown CI image') from error
        commit='unknown' # Building an extracted source archive is supported.
    return {'commit':commit,'source_digest':digest}
if __name__=='__main__':
    value=identity()
    content='#pragma once\n'+''.join(f'#define LDG_{key.upper() if key!="commit" else "BUILD_COMMIT"} {json.dumps(text)}\n' for key,text in value.items())
    path=Path(sys.argv[1]);path.parent.mkdir(parents=True,exist_ok=True)
    if not path.exists() or path.read_text()!=content:path.write_text(content)
    evidence={'format':2,**value,'inputs':manifest(),
        'raw_inputs':{p.relative_to(ROOT).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs()},
        'resolved_locks':{p.name:resolved_lock(p.read_bytes()) for p in ROOT.glob('dependencies.*.lock')}}
    path.with_suffix('.json').write_text(json.dumps(evidence,indent=2)+'\n')
