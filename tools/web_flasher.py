#!/usr/bin/env python3
"""Prepare a validated native-IDF USB catalog. Never access a serial device."""
from pathlib import Path
import argparse, hashlib, json, re, shutil, struct, subprocess, zipfile
from firmware_identity import identity, ROOT

PROFILES=json.loads((ROOT/'web-flasher/profiles.json').read_text())['profiles']
def sha(data):return hashlib.sha256(data).hexdigest()
def image(data,chip):
    if len(data)<32 or data[0]!=0xe9 or struct.unpack_from('<H',data,12)[0]!=chip:raise ValueError('wrong chip or image format')
    if data[23]!=1 or sha(data[:-32])!=data[-32:].hex():raise ValueError('image SHA-256 mismatch')
def partitions(data):
    result=[]
    for pos in range(0,len(data),32):
        magic,kind,subtype,offset,size,label,flags=struct.unpack_from('<HBBII16sI',data,pos)
        if magic==0xffff:break
        if magic==0xebeb:
            if hashlib.md5(data[:pos]).digest()!=data[pos+16:pos+32]:raise ValueError('partition MD5 mismatch')
            continue
        if magic!=0x50aa:raise ValueError('invalid partition entry')
        result.append((kind,subtype,offset,size,label.rstrip(b'\0').decode()))
    if not result:raise ValueError('empty partition table')
    ordered=sorted(result,key=lambda p:p[2])
    if any(a[2]+a[3]>b[2] for a,b in zip(ordered,ordered[1:])):raise ValueError('overlapping partitions')
    return result

def static_ram(elf):
    if elf[:5]!=b'\x7fELF\x01':raise ValueError('ELF32 required')
    offset=struct.unpack_from('<I',elf,32)[0];size,count,strings=struct.unpack_from('<HHH',elf,46)
    sections=[struct.unpack_from('<IIIIIIIIII',elf,offset+i*size) for i in range(count)]
    table=sections[strings];names=elf[table[4]:table[4]+table[5]]
    return sum(s[5] for s in sections if names[s[0]:].split(b'\0',1)[0] in [b'.dram0.data',b'.dram0.bss',b'.noinit'])

def profile_build(profile,base,expected):
    board=profile['id'];build=base/('build-'+board);args=json.loads((build/'flasher_args.json').read_text())
    chip='ESP32-S3' if board=='guition' else 'ESP32';chipid=9 if board=='guition' else 0
    flash=16*1024*1024 if board=='guition' else 4*1024*1024
    if args['extra_esptool_args']['chip']!=chip.lower().replace('-',''):raise ValueError('flasher chip mismatch')
    entries=[]
    for offset,file in args['flash_files'].items():
        path=Path(file)
        if path.is_absolute() or '..' in path.parts:raise ValueError('unsafe binary path')
        data=(build/path).read_bytes();entries.append((int(offset,0),path.name,data))
    if len(entries)!=4 or len({n for _,n,_ in entries})!=4:raise ValueError('exactly four distinct USB parts required')
    byname={name:(offset,data) for offset,name,data in entries}
    app_offset,app=byname['ldgcontrol.bin'];boot_offset,boot=byname['bootloader.bin']
    if boot_offset!=(0 if chipid==9 else 0x1000):raise ValueError('wrong bootloader offset')
    image(app,chipid);image(boot,chipid)
    metadata=re.findall(rb'LDG_METADATA_V1:([^\n\x00]+)\n',app)
    if len(metadata)!=1:raise ValueError('missing or ambiguous build identity')
    meta=json.loads(metadata[0]);
    if any(meta.get(key)!=value for key,value in {**expected,'board':board}.items()):raise ValueError('stale or wrong-board firmware identity')
    table_offset,table=byname['partition-table.bin'];parts=partitions(table)
    if table_offset!=0x8000:raise ValueError('wrong partition-table offset')
    apps=[p for p in parts if p[0]==0 and p[1] in (0x10,0x11)]
    if len(apps)!=2 or apps[0][2]!=app_offset or len({p[3] for p in apps})!=1:raise ValueError('two matching OTA slots required')
    slot=apps[0][3];limit=0x600000 if chipid==9 else 0x190000
    if chipid==0 and slot!=0x1c0000:raise ValueError('wrong classic ESP32 OTA layout')
    if len(app)>limit or len(app)>slot:raise ValueError('application size budget exceeded')
    ota=[p for p in parts if p[:2]==(1,0)];nvs=[p for p in parts if p[:2]==(1,2)]
    if len(ota)!=1 or len(nvs)!=1 or byname['ota_data_initial.bin'][0]!=ota[0][2] or len(byname['ota_data_initial.bin'][1])>ota[0][3]:raise ValueError('wrong OTA data/NVS layout')
    ordered=sorted(entries)
    if any(a[0]+len(a[2])>b[0] for a,b in zip(ordered,ordered[1:])):raise ValueError('overlapping USB parts')
    for offset,name,data in entries:
        if offset+len(data)>flash:raise ValueError('part exceeds flash size')
        if offset<nvs[0][2]+nvs[0][3] and offset+len(data)>nvs[0][2]:raise ValueError('USB part would overwrite NVS')
    return entries,{'version':meta['version'],**expected,'app_bytes':len(app),'app_limit_bytes':limit,'ota_slot_bytes':slot,'static_ram_bytes':static_ram((build/'ldgcontrol.elf').read_bytes()),'capabilities':{key:True for key in ['usb_provisioning','wifi','web','mqtt','mqtt_tls','home_assistant_discovery','tci','ota']}|{'display':board!='headless'}},chip

def package(output,base,allow_missing=False,allow_dirty=False):
    expected=identity()
    if not allow_dirty and subprocess.check_output(['git','status','--porcelain','--untracked-files=normal'],cwd=ROOT):raise ValueError('publish only from a clean checkout')
    if output.exists():raise ValueError('output directory already exists; choose a fresh path')
    output.mkdir(parents=True)
    for name in ['index.html','app.js','style.css','provisioning.js','serial-api.js','i18n.js','photos.json']:
        shutil.copy2(ROOT/'web-flasher'/name,output/name)
    shutil.copytree(ROOT/'web-flasher/images',output/'images')
    catalog={'schema':1,'commit':expected['commit'],'profiles':[]}
    for source in PROFILES:
        profile=dict(source);profile['documentation']='https://github.com/ra3vld/LDGControl-IDF/blob/'+expected['commit']+'/'+profile.pop('docs')
        try:entries,build,chip=profile_build(profile,base,expected)
        except FileNotFoundError:
            if not allow_missing:raise
            entries=[]
        if entries:
            dest=output/'firmware'/profile['id'];dest.mkdir(parents=True)
            manifest={'name':'LDGControl-IDF '+profile['title'],'version':build['version'],'new_install_prompt_erase':True,'new_install_improv_wait_time':0,'builds':[{'chipFamily':chip,'parts':[]}]}
            checks={}
            for offset,name,data in sorted(entries):
                (dest/name).write_bytes(data);manifest['builds'][0]['parts'].append({'path':name,'offset':offset});checks[name]={'bytes':len(data),'sha256':sha(data),'offset':offset}
            (dest/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');(dest/'checksums.json').write_text(json.dumps(checks,indent=2)+'\n')
            with zipfile.ZipFile(dest/'usb-firmware.zip','w',zipfile.ZIP_DEFLATED) as archive:
                for p in sorted(dest.iterdir()):
                    if p.suffix!='.zip':archive.write(p,p.name)
            profile['build']=build
        catalog['profiles'].append(profile)
    (output/'catalog.json').write_text(json.dumps(catalog,indent=2)+'\n')
    # Only tracked source files; ignored local notes, credentials and dependencies never enter the archive.
    files=subprocess.check_output(['git','ls-files','-z'],cwd=ROOT).decode().split('\0')
    with zipfile.ZipFile(output/'sources.zip','w',zipfile.ZIP_DEFLATED) as archive:
        for name in sorted(filter(None,files)):
            if name.startswith(('local-notes/','build-','web/node_modules/','web-flasher/node_modules/')) or name.endswith('.pem') and name!='release-public.pem':raise ValueError('private or generated source entry')
            archive.write(ROOT/name,'LDGControl-IDF/'+name)
    shutil.copy2(ROOT/'LICENSE',output/'LICENSE')
    return catalog
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);p.add_argument('--build-root',type=Path,default=ROOT);p.add_argument('--allow-missing',action='store_true');p.add_argument('--allow-dirty',action='store_true');args=p.parse_args()
    result=package(args.output.resolve(),args.build_root.resolve(),args.allow_missing,args.allow_dirty)
    print('Catalog:',len([p for p in result['profiles'] if p.get('build')]),'validated profiles')
