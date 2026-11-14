"""Reject incorrect USB images, partitions and source identity using synthetic files."""
from pathlib import Path
import hashlib,json,struct,sys,tempfile,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import web_flasher as f

def image(chip=0,payload=b'payload'):
    header=bytearray(24);header[0]=0xe9;header[23]=1;struct.pack_into('<H',header,12,chip)
    data=bytes(header)+payload;return data+hashlib.sha256(data).digest()
def table():
    result=b''
    for kind,sub,offset,size,label in [(1,2,0x9000,0x5000,b'nvs'),(1,0,0xe000,0x2000,b'otadata'),(0,0x10,0x10000,0x1c0000,b'ota_0'),(0,0x11,0x1d0000,0x1c0000,b'ota_1')]:result+=struct.pack('<HBBII16sI',0x50aa,kind,sub,offset,size,label,0)
    return result+bytes.fromhex('ebeb')+b'\xff'*14+hashlib.md5(result).digest()+b'\xff'*32
class Catalog(unittest.TestCase):
 def test_integrity_and_chip(self):
    f.image(image(),0)
    for binary,chip in [(image(9),0),(b'bad',0),(image()[:-1]+b'x',0)]:
      with self.assertRaises(ValueError):f.image(binary,chip)
 def test_partition_integrity(self):
    self.assertEqual(len(f.partitions(table())),4)
    broken=bytearray(table());broken[10]^=1
    with self.assertRaisesRegex(ValueError,'MD5'):f.partitions(broken)
 def test_catalog_identity_and_nvs_preservation(self):
    with tempfile.TemporaryDirectory() as folder:
      base=Path(folder);build=base/'build-headless';build.mkdir();expected={'commit':'1'*40,'source_digest':'2'*64};meta=dict(expected,board='headless',version='0.1.0')
      app=image(payload=b'LDG_METADATA_V1:'+json.dumps(meta).encode()+b'\n')
      for name,data in [('ldgcontrol.bin',app),('bootloader.bin',image()),('partition-table.bin',table()),('ota_data_initial.bin',b'\xff'*0x2000)]:(build/name).write_bytes(data)
      # Minimal ELF with an empty section table.
      elf=bytearray(52);elf[:5]=b'\x7fELF\x01';struct.pack_into('<I',elf,32,52);struct.pack_into('<HHH',elf,46,40,1,0);elf+=b'\0'*40;(build/'ldgcontrol.elf').write_bytes(elf)
      args={'extra_esptool_args':{'chip':'esp32'},'flash_files':{'0x1000':'bootloader.bin','0x10000':'ldgcontrol.bin','0x8000':'partition-table.bin','0xe000':'ota_data_initial.bin'}};(build/'flasher_args.json').write_text(json.dumps(args))
      entries,details,chip=f.profile_build({'id':'headless'},base,expected);self.assertEqual(chip,'ESP32');self.assertEqual(details['app_bytes'],len(app));self.assertEqual(len(entries),4)
      with self.assertRaisesRegex(ValueError,'identity'):f.profile_build({'id':'headless'},base,dict(expected,commit='3'*40))
      args['flash_files']['0x9000']=args['flash_files'].pop('0x1000');(build/'flasher_args.json').write_text(json.dumps(args))
      with self.assertRaises(ValueError):f.profile_build({'id':'headless'},base,expected)
if __name__=='__main__':unittest.main()
