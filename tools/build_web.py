#!/usr/bin/env python3
"""Compile TypeScript; embed deterministic, hashed gzip assets in the app."""
import gzip, hashlib, json, re, shutil, subprocess, sys
from pathlib import Path
from web_tools import ensure_tools, bootstrap_lock
root=Path(__file__).resolve().parents[1]
web=root/'web'
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
with bootstrap_lock(root):
    node,version,npm=ensure_tools(root)
    lock=web/'package-lock.json'
    if not lock.exists():raise SystemExit('Missing web/package-lock.json')
    marker=web/'node_modules/.ldg-lock-sha256'
    digest=hashlib.sha256(lock.read_bytes()).hexdigest()
    if not marker.exists() or marker.read_text()!=digest or not (web/'node_modules/typescript/bin/tsc').exists() or not (web/'node_modules/terser/bin/terser').exists():
        subprocess.run([node,npm,'ci','--cache',str(root/'.tools/npm-cache'),'--ignore-scripts','--no-audit','--no-fund'],cwd=web,check=True)
        marker.write_text(digest)
subprocess.run([node,str(web/'node_modules/typescript/bin/tsc')],cwd=web,check=True)
files={p.name:p.read_bytes() for p in (web/'dist').glob('*.js')}
files['style.css']=(web/'src/style.css').read_bytes()
for name in list(files):
    if name.endswith('.js'):
        result=subprocess.run([node,str(web/'node_modules/terser/bin/terser'),'--module','--compress','--mangle'],input=files[name],capture_output=True,check=True)
        files[name]=result.stdout
# Version all assets as one coherent set so module imports have stable hashed paths.
build=hashlib.sha256(b''.join(files[k] for k in sorted(files))).hexdigest()[:12]
html=(web/'src/index.html').read_text().replace('@BUILD@',build).encode()
assets={'/':('text/html; charset=utf-8',html)}
for name,data in files.items():
    assets['/assets/'+build+'/'+name]=('text/css' if name.endswith('.css') else 'text/javascript',data)
lines=['#pragma once','#include <cstring>','#include <cstddef>','namespace web {','struct Asset {const char* path;const char* type;const unsigned char* data;size_t size;};']
entries=[];total=0
for i,(path,(mime,raw)) in enumerate(assets.items()):
    data=gzip.compress(raw,compresslevel=9,mtime=0);total+=len(data)
    preview=out/'public'/('index.html' if path=='/' else path.lstrip('/'))
    preview.parent.mkdir(parents=True,exist_ok=True);preview.write_bytes(raw)
    lines.append('static const unsigned char a%d[]={%s};'%(i,','.join(map(str,data))))
    entries.append('{%s,%s,a%d,sizeof(a%d)}'%(json.dumps(path),json.dumps(mime),i,i))
lines+=['static const Asset assets[]={'+','.join(entries)+'};','inline const Asset* find(const char* path){for(const auto& a:assets)if(!strcmp(path,a.path))return &a;return nullptr;}','}']
content='\n'.join(lines)+'\n';target=out/'web_assets.hpp'
if not target.exists() or target.read_text()!=content:target.write_text(content)
(out/'web-assets.json').write_text(json.dumps({'hash':build,'gzip_bytes':total,'assets':list(assets)},indent=2)+'\n')
print(f'Embedded {len(assets)} assets, {total} gzip bytes, {build}')
