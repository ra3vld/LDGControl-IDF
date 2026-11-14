#!/usr/bin/env python3
"""Verify a release package before installation; does not contact a device."""
import argparse,base64,hashlib,json,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--manifest',type=Path,required=True);p.add_argument('--image',type=Path,required=True);p.add_argument('--public-key',type=Path,default=Path(__file__).resolve().parents[1]/'release-public.pem');a=p.parse_args()
m=json.loads(a.manifest.read_text());data=a.image.read_bytes()
if m.get('format')!=1 or m.get('board') not in ('headless','sunton','guition'):raise SystemExit('Invalid manifest format or board')
if len(data)!=m['size'] or hashlib.sha256(data).hexdigest()!=m['sha256']:raise SystemExit('Image length or SHA-256 mismatch')
payload=f"LDGControl-IDF\n1\n{m['board']}\n{m['version']}\n{m['size']}\n{m['sha256']}\n".encode()
with tempfile.TemporaryDirectory() as temporary:
    directory=Path(temporary);unsigned=directory/'payload';signature=directory/'signature';unsigned.write_bytes(payload);signature.write_bytes(base64.b64decode(m['signature'],validate=True))
    subprocess.run(['openssl','dgst','-sha256','-verify',str(a.public_key),'-signature',str(signature),str(unsigned)],check=True)
print(f"{m['board']} {m['version']}: signature, size and SHA-256 verified")
