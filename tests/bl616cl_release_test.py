"""Release corruption, partition drift, target binding and failed writer gates."""
import copy
import importlib.util
import json
import shutil
import subprocess
import struct
import sys
import tempfile
import types
import unittest
import zlib
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import bl616cl_release as r
import update_bl616cl_release as u

class ReleaseTest(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.pkg=self.root/'pkg';self.pkg.mkdir()
        rows=[]
        for name,(a0,s0,a1,s1) in r.LAYOUT.items():
            kind={'Boot2':16,'FW':0,'mfg':10,'media':2,'PSM':3,'KEY':4,'DATA':5,'factory':6,'RA4_OTA':7}[name]
            rows.append(bytes([kind,0,0])+name.encode().ljust(9,b'\0')+struct.pack('<6I',a0,a1,s0,s1,0,0))
        head=struct.pack('<IHHI',0x54504642,0,9,0);entries=b''.join(rows)
        table=head+struct.pack('<I',zlib.crc32(head))+entries+struct.pack('<I',zlib.crc32(entries))
        for name,data in [('partition.bin',table),('boot2.bin',b'BFNP'+bytes(32)),
                          ('application.bin',b'BFNP'+b'0.6.0\0'+bytes(32))]:
            (self.pkg/name).write_bytes(data)
        self.manifest={'schema':1,'chip':'bl616cl','board':r.BOARD,'layout':'bridge-v1-4MiB',
                       'target_serial':r.SERIAL,'usb':{'vid':0x2341,'pid':0x1002,'interface':0},
                       'firmware_version':'0.6.0','images':{role:{'file':name,'address':addr} for role,(name,addr,_) in r.IMAGES.items()},'files':{}}
        self.save()
        self.binding={'schema':1,'serial':r.SERIAL,'chip':'bl616cl','host':sys.platform,'location':'3-1'}
        self.bp=self.root/'binding.json';self.bp.write_text(json.dumps(self.binding))
        self.tool=self.root/'tool';self.tool.touch()
    def save(self):
        for p in self.pkg.rglob('*'):
            if p.is_file() and p.name!='release.json':self.manifest['files'][p.relative_to(self.pkg).as_posix()]={'bytes':p.stat().st_size,'sha256':r.digest(p.read_bytes())}
        (self.pkg/'release.json').write_text(json.dumps(self.manifest))
    def test_valid(self):r.verify(self.pkg)
    def test_hash_list_cannot_authorize_debug_or_private_artifacts(self):
        for name in ('firmware.elf','firmware.map','firmware.cpp','runtime-manifest.json',
                     'evidence/local.json','logs/session.log','original-full-flash.bin'):
            path=self.pkg/name;path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(b'private test artifact');self.save()
            with self.assertRaisesRegex(ValueError,'non-public'):r.verify(self.pkg)
            path.unlink();self.manifest['files'].pop(name,None);self.save()

    def test_corruption_before_device(self):
        (self.pkg/'application.bin').write_bytes(b'bad')
        with patch.object(u,'bootrom') as device,patch.object(u.subprocess,'run') as writer:
            with self.assertRaises(ValueError):u.flash(self.pkg,'COM1',self.bp,self.tool,3,1)
            device.assert_not_called();writer.assert_not_called()
    def test_manifest_targets_paths_capacity(self):
        original=copy.deepcopy(self.manifest)
        for field,value in [('chip','esp32s3'),('board','bl616cldk'),('firmware_version','0.6.1')]:
            self.manifest=copy.deepcopy(original);self.manifest[field]=value;self.save()
            with self.assertRaises(ValueError):r.verify(self.pkg)
        self.manifest=original;self.save()
        self.manifest['images']['application']['address']=0x1c0000;self.save()
        with self.assertRaises(ValueError):r.verify(self.pkg)
        self.manifest=copy.deepcopy(original);self.manifest['files']['../escape']={'bytes':0,'sha256':r.digest(b'')}
        (self.pkg/'release.json').write_text(json.dumps(self.manifest))
        with self.assertRaises(ValueError):r.verify(self.pkg)
    def test_crc_and_layout(self):
        b=bytearray((self.pkg/'partition.bin').read_bytes());b[20]^=1
        (self.pkg/'partition.bin').write_bytes(b);self.save()
        with self.assertRaisesRegex(ValueError,'CRC'):r.verify(self.pkg)
        # Recomputed CRC still must not authorize writing into RA4 cache.
        b[20]^=1;struct.pack_into('<I',b,16+36+12,0x1c0000)
        struct.pack_into('<I',b,len(b)-4,zlib.crc32(b[16:-4]))
        (self.pkg/'partition.bin').write_bytes(b);self.save()
        with self.assertRaisesRegex(ValueError,'layout'):r.verify(self.pkg)
    def test_wrong_port_and_location(self):
        p=types.SimpleNamespace(vid=0xffff,pid=0xffff,location='3-2',device='COM1')
        with patch.object(u,'selected_port',return_value=p),patch.object(u,'location',side_effect=lambda p:p.location):
            with self.assertRaises(ValueError):u.bootrom('COM1',self.binding)
            p.location='3-1';p.vid=0x2341;p.pid=0x1002
            with self.assertRaises(ValueError):u.bootrom('COM1',self.binding)
    def test_retry_requires_device_sha(self):
        outputs=[types.SimpleNamespace(returncode=0,stdout=b'100% done'),
                 types.SimpleNamespace(returncode=0,stdout=b'Verification succeeded\n'*4)]
        with patch.object(u,'bootrom') as check,patch.object(u.subprocess,'run',side_effect=outputs) as writer:
            u.flash(self.pkg,'COM1',self.bp,self.tool,3,1)
            self.assertEqual(writer.call_count,2);self.assertEqual(check.call_count,3)
        with patch.object(u,'bootrom'),patch.object(u.subprocess,'run',return_value=outputs[0]) as writer:
            with self.assertRaises(ValueError):u.flash(self.pkg,'COM1',self.bp,self.tool,3,1)
            self.assertEqual(writer.call_count,3)
    def test_no_write_on_mismatched_board(self):
        with patch.object(u,'bootrom',side_effect=ValueError('wrong location')),patch.object(u.subprocess,'run') as writer:
            with self.assertRaises(ValueError):u.flash(self.pkg,'COM1',self.bp,self.tool,3,1)
            writer.assert_not_called()
    def test_extra_and_oversized_files(self):
        extra=self.pkg/'unexpected.bin';extra.write_bytes(b'extra')
        with self.assertRaisesRegex(ValueError,'unlisted'):r.verify(self.pkg)
        extra.unlink()
        (self.pkg/'application.bin').write_bytes(b'BFNP0.6.0\0'+bytes(0x168000))
        self.save()
        with self.assertRaisesRegex(ValueError,'capacity'):r.verify(self.pkg)
    def test_standalone_entry_keeps_package_sealed(self):
        for name in ('bl616cl_release.py','update_bl616cl_release.py'):
            shutil.copyfile(Path(u.__file__).parent/name,self.pkg/name)
        self.save()
        result=subprocess.run([sys.executable,str(self.pkg/'update_bl616cl_release.py'),
                               'verify',str(self.pkg)],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertFalse((self.pkg/'__pycache__').exists())
        r.verify(self.pkg)
    def test_wsl_physical_location_and_ambiguity(self):
        port=types.SimpleNamespace(vid=0x2341,pid=0x1002)
        device={'BusId':'3-1','IsWslAttached':True,
                'InstanceId':'USB\\VID_2341&PID_1002\\'+r.SERIAL}
        with patch.object(u.sys,'platform','linux'),patch.object(u.Path,'exists',return_value=True),patch.object(u.Path,'read_text',return_value='microsoft-standard'),patch.object(u.subprocess,'check_output',return_value=json.dumps({'Devices':[device]})) as query:
            self.assertEqual(u.location(port),'wsl-usbipd:3-1')
            self.assertEqual(query.call_args.kwargs['timeout'],15)
            query.return_value=json.dumps({'Devices':[device,dict(device,BusId='3-2')]})
            with self.assertRaisesRegex(ValueError,'uniquely'):u.location(port)
            query.side_effect=u.subprocess.TimeoutExpired('usbipd',15)
            with self.assertRaises(u.subprocess.TimeoutExpired):u.location(port)

if __name__=='__main__':unittest.main()
