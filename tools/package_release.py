#!/usr/bin/env python3
"""Sign exact app images; the private key is external to the repository."""
import argparse, base64, hashlib, json, subprocess, tempfile, re
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--board',required=True,choices=['headless','sunton','guition']);p.add_argument('--version',required=True);p.add_argument('--image',type=Path,required=True);p.add_argument('--key',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
if not re.fullmatch(r'\d+\.\d+\.\d+',a.version) or len(a.version)>31:raise SystemExit('Invalid version')
data=a.image.read_bytes()
if len(data)<256 or data[0]!=0xe9 or data[32:36]!=bytes.fromhex('3254cdab'):raise SystemExit('Not an ESP-IDF app image')
if data[48:80].split(b'\0')[0].decode()!=a.version:raise SystemExit('Manifest version must match the embedded application version')
if len(data)>(0x600000 if a.board=='guition' else 0x190000):raise SystemExit('Image budget exceeded')
if int.from_bytes(data[12:14],'little')!=(9 if a.board=='guition' else 0):raise SystemExit('Wrong chip for board')
sha=hashlib.sha256(data).hexdigest();payload=f'LDGControl-IDF\n1\n{a.board}\n{a.version}\n{len(data)}\n{sha}\n'.encode()
with tempfile.TemporaryDirectory() as tmp:
    unsigned=Path(tmp)/'manifest';unsigned.write_bytes(payload)
    signature=subprocess.check_output(['openssl','dgst','-sha256','-sign',str(a.key),str(unsigned)])
a.out.mkdir(parents=True,exist_ok=True)
(a.out/(a.board+'.bin')).write_bytes(data)
(a.out/(a.board+'.manifest.json')).write_text(json.dumps({'format':1,'board':a.board,'version':a.version,'size':len(data),'sha256':sha,'signature':base64.b64encode(signature).decode()},indent=2)+'\n')

# Paste this block into the GitHub release body; the browser can obtain the
# signed manifests through the REST JSON response without asset CORS redirects.
manifests={p.stem.split('.')[0]:json.loads(p.read_text()) for p in sorted(a.out.glob('*.manifest.json'))}
(a.out/'release-notes.md').write_text('LDGControl-IDF '+a.version+'\n\n<!-- ldg-manifests\n'+json.dumps(manifests,separators=(',',':'))+'\n-->\n')
