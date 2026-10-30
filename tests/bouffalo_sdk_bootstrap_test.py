#!/usr/bin/env python3
"""Exercise pinned bootstrap and preservation against real, local Git repositories."""
import importlib.util,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('sdk_prepare',ROOT/'tools/prepare_bouffalo_sdk_source.py');sdk=importlib.util.module_from_spec(spec);spec.loader.exec_module(sdk)
def git(p,*args):return subprocess.check_output(['git','-C',str(p),*args],text=True,stderr=subprocess.DEVNULL).strip()
class Bootstrap(unittest.TestCase):
 def setUp(self):
  (ROOT/'build').mkdir(exist_ok=True)
  self.tmp=tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='sdk-bootstrap-test-');self.base=Path(self.tmp.name)
  self.source=self.base/'remote';self.source.mkdir();git(self.source,'init')
  (self.source/'input').write_text('locked\n');git(self.source,'add','input');git(self.source,'-c','user.name=fixture','-c','user.email=fixture@localhost','commit','-m','locked');self.commit=git(self.source,'rev-parse','HEAD')
  (self.source/'input').write_text('newer HEAD\n');git(self.source,'add','input');git(self.source,'-c','user.name=fixture','-c','user.email=fixture@localhost','commit','-m','later')
  self.parent=self.base/'parent';self.parent.mkdir();git(self.parent,'init');git(self.parent,'update-index','--add','--cacheinfo','160000,'+self.commit+',vendor/sdk')
  self.dest=self.parent/'vendor/sdk';self.dest.mkdir(parents=True)
  self.old=sdk.ROOT_DIR;sdk.ROOT_DIR=self.parent
  self.lock={'path':'vendor/sdk','commit':self.commit,'source_url':self.source.as_uri()}
 def tearDown(self):sdk.ROOT_DIR=self.old;self.tmp.cleanup()
 def test_fresh_empty_gitlink_fetches_older_locked_commit(self):
  sdk.ensure_root_submodule(self.lock,self.dest)
  self.assertEqual(git(self.dest,'rev-parse','HEAD'),self.commit);self.assertEqual((self.dest/'input').read_text(),'locked\n')
 def test_dirty_existing_checkout_is_preserved(self):
  sdk.ensure_root_submodule(self.lock,self.dest);(self.dest/'input').write_text('user work\n')
  with self.assertRaisesRegex(RuntimeError,'tracked local changes'):sdk.ensure_root_submodule(self.lock,self.dest)
  self.assertEqual((self.dest/'input').read_text(),'user work\n')
 def test_nonempty_nonrepository_is_preserved(self):
  (self.dest/'user').write_text('keep')
  with self.assertRaisesRegex(RuntimeError,'not a Git checkout'):sdk.ensure_root_submodule(self.lock,self.dest)
  self.assertEqual((self.dest/'user').read_text(),'keep');self.assertFalse((self.dest/'.git').exists())
if __name__=='__main__':unittest.main()
