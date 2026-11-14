#!/usr/bin/env python3
"""Validate actual managed code against the pinned portable dependency contract."""
from pathlib import Path
import argparse,json,yaml
from idf_component_tools.hash_tools.validate import validate_hash_eq_hashdir
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--board',choices=['headless','sunton','guition'],required=True);args=p.parse_args()
target='esp32s3' if args.board=='guition' else 'esp32';build=ROOT/('build-'+args.board)
expected=yaml.safe_load((ROOT/('dependencies.'+target+'.lock')).read_text())
actual=yaml.safe_load((build/'dependencies.lock').read_text())
if set(expected['dependencies'])!=set(actual['dependencies']):raise SystemExit('Resolved component set differs from the pinned contract')
verified={}
for name,locked in expected['dependencies'].items():
    resolved=actual['dependencies'][name]
    if str(resolved['version'])!=str(locked['version']):raise SystemExit('Resolved version mismatch: '+name)
    if locked['source']['type']=='idf':
        if resolved['source']['type']!='idf':raise SystemExit('Unexpected SDK source')
        continue
    folder=(ROOT/'managed_components'/name.replace('/','__')).resolve()
    source=resolved['source']
    if source['type']=='local':
        location=Path(source['path']);location=location if location.is_absolute() else ROOT/location
        if location.resolve()!=folder:raise SystemExit('Unexpected local component override: '+name)
    elif source['type']=='service':
        if resolved.get('component_hash')!=locked['component_hash']:raise SystemExit('Resolved hash mismatch: '+name)
    else:raise SystemExit('Unsupported component source: '+name)
    # Recompute the package hash, rather than trusting a copied marker file.
    validate_hash_eq_hashdir(folder,locked['component_hash'])
    verified[name]={'version':str(locked['version']),'sha256':locked['component_hash']}
output=build/'generated/components-verified.json';output.parent.mkdir(exist_ok=True,parents=True)
output.write_text(json.dumps({'target':target,'components':verified},indent=2)+'\n')
print('Verified full contents of',len(verified),'pinned managed components')
