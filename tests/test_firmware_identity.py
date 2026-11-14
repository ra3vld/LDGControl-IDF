"""Publication identity remains stable across SDK passes, but binds actual code."""
import copy,json,os,subprocess,sys,unittest
from pathlib import Path
import yaml
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import firmware_identity as f
class Fingerprint(unittest.TestCase):
 def test_lock_configure_metadata_does_not_change_resolved_code(self):
    original=yaml.safe_load((f.ROOT/'dependencies.esp32.lock').read_text())
    changed=copy.deepcopy(original);changed['manifest_hash']='different-configure-pass'
    for value in changed['dependencies'].values():
        if 'registry_url' in value['source']:value['source']['registry_url']=value['source']['registry_url'].rstrip('/')+'/'
        value['dependencies']=[{'name':'descriptive-rule','version':'unused registry metadata'}]
    self.assertEqual(f.resolved_lock(yaml.safe_dump(original)),f.resolved_lock(yaml.safe_dump(changed)))
    first=next(v for v in changed['dependencies'].values() if v['source']['type']=='service');first['component_hash']='f'*64
    self.assertNotEqual(f.resolved_lock(yaml.safe_dump(original)),f.resolved_lock(yaml.safe_dump(changed)))
 def test_target_version_and_corrupt_hash_remain_bound(self):
    original=yaml.safe_load((f.ROOT/'dependencies.esp32.lock').read_text());changed=copy.deepcopy(original);changed['target']='esp32s3'
    self.assertNotEqual(f.resolved_lock(yaml.safe_dump(original)),f.resolved_lock(yaml.safe_dump(changed)))
    changed=copy.deepcopy(original);first=next(v for v in changed['dependencies'].values() if v['source']['type']=='service');first['version']='999.0.0'
    self.assertNotEqual(f.resolved_lock(yaml.safe_dump(original)),f.resolved_lock(yaml.safe_dump(changed)))
    first['component_hash']='invalid'
    with self.assertRaises(ValueError):f.resolved_lock(yaml.safe_dump(changed))
 def test_known_checkout_with_different_container_owner(self):
    previous=os.environ.get('GIT_TEST_ASSUME_DIFFERENT_OWNER');os.environ['GIT_TEST_ASSUME_DIFFERENT_OWNER']='1'
    try:
      before=subprocess.run(['git','-c','safe.directory=','rev-parse','HEAD'],cwd=f.ROOT,capture_output=True)
      self.assertNotEqual(before.returncode,0,'Owner mismatch must reproduce without scoped trust')
      self.assertRegex(f.git('rev-parse','HEAD'),r'^[0-9a-f]{40}$')
    finally:
      if previous is None:os.environ.pop('GIT_TEST_ASSUME_DIFFERENT_OWNER',None)
      else:os.environ['GIT_TEST_ASSUME_DIFFERENT_OWNER']=previous
if __name__=='__main__':unittest.main()
