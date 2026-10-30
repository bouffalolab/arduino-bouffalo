#!/usr/bin/env python3
"""Build and verify a local BL616CL development release directory (stdlib only)."""
import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BOARD = 'unor4_bl616cl_provisional'
SERIAL = 'BL616CL-PROVISIONAL'
LAYOUT = {'Boot2': (0, 0xE000, 0, 0), 'FW': (0x10000, 0x1B0000, 0x210000, 0x168000),
          'mfg': (0x210000, 0x168000, 0, 0), 'RA4_OTA': (0x1C0000, 0x50000, 0, 0),
          'media': (0x378000, 0x71000, 0, 0), 'PSM': (0x3E9000, 0x8000, 0, 0),
          'KEY': (0x3F1000, 0x2000, 0, 0), 'DATA': (0x3F3000, 0x5000, 0, 0),
          'factory': (0x3F8000, 0x8000, 0, 0)}
IMAGES = {'boot2': ('boot2.bin', 0, 0xE000),
          'partition': ('partition.bin', 0xE000, 0x1000),
          'application': ('application.bin', 0x10000, 0x168000 - 32)}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def partition_layout(data):
    require(len(data) >= 20 and data[:4] == b'BFPT', 'invalid partition magic/size')
    _, version, count, age, crc = struct.unpack('<IHHII', data[:16])
    require(version == 0 and count == len(LAYOUT) and age == 0, 'not a factory v1 partition table')
    require(len(data) == 16 + count * 36 + 4, 'partition entry count/length mismatch')
    require(zlib.crc32(data[:12]) == crc, 'partition header CRC mismatch')
    require(zlib.crc32(data[16:-4]) == struct.unpack('<I', data[-4:])[0], 'partition entries CRC mismatch')
    entries = {}
    for index in range(count):
        row = data[16 + index * 36:52 + index * 36]
        kind, device, active = row[:3]
        name = row[3:12].split(b'\0', 1)[0].decode('ascii')
        a0, a1, s0, s1, length, entry_age = struct.unpack('<6I', row[12:])
        require(name not in entries and device == 0 and active == 0 and length == 0 and entry_age == 0,
                'duplicate or non-factory partition entry')
        require(kind == {'Boot2':16,'FW':0,'mfg':10,'media':2,'PSM':3,'KEY':4,'DATA':5,
                         'factory':6,'RA4_OTA':7}.get(name), 'partition type mismatch')
        entries[name] = (a0, s0, a1, s1)
    require(entries == LAYOUT, 'partition differs from frozen bridge layout v1')


def safe_file(directory, name):
    require(isinstance(name, str) and name and '\\' not in name, 'invalid package path')
    relative = Path(name)
    require(not relative.is_absolute() and '..' not in relative.parts, 'unsafe package path')
    path = directory / relative
    require(not any(p.is_symlink() for p in [path, *path.parents]), 'symlinks forbidden in package')
    require(path.is_file(), 'missing package file: ' + name)
    require(path.resolve().is_relative_to(directory.resolve()), 'package path escapes directory')
    return path


PUBLIC_PACKAGE_FILES = {name for name, _, _ in IMAGES.values()} | {
    'README.md', 'bl616cl_release.py', 'update_bl616cl_release.py',
    'source-manifest.json', 'proprietary-manifest.json',
}


def public_package_file(name):
    return name in PUBLIC_PACKAGE_FILES or (name.startswith('licenses/') and
        Path(name).suffix.lower() in {'.txt', '.md', '.json'})


def verify(directory):
    directory = directory.resolve()
    manifest = json.loads(safe_file(directory, 'release.json').read_text())
    require(manifest.get('schema') == 1 and manifest.get('chip') == 'bl616cl' and
            manifest.get('board') == BOARD and manifest.get('layout') == 'bridge-v1-4MiB',
            'wrong chip, board, schema or layout')
    require(manifest.get('target_serial') == SERIAL and manifest.get('usb') == {'vid':0x2341,'pid':0x1002,'interface':0},
            'wrong target USB contract')
    version = manifest.get('firmware_version', '')
    require(re.fullmatch(r'\d+\.\d+\.\d+', version), 'invalid firmware version')
    require(set(manifest.get('images', {})) == set(IMAGES), 'exactly three images required')
    listed = manifest.get('files', {})
    require(isinstance(listed, dict) and listed, 'missing package file hashes')
    for name, record in listed.items():
        require(public_package_file(name), 'non-public package file: ' + name)
        data = safe_file(directory, name).read_bytes()
        require(record == {'bytes':len(data), 'sha256':digest(data)}, 'file hash/size mismatch: ' + name)
    for role, (name, address, limit) in IMAGES.items():
        record = manifest['images'][role]
        require(record == {'file':name, 'address':address}, 'wrong image address/name: ' + role)
        require(name in listed, 'image not covered by hash manifest')
        data = safe_file(directory, name).read_bytes()
        require(0 < len(data) <= limit, 'image capacity exceeded: ' + role)
        if role == 'partition':
            partition_layout(data)
        else:
            require(data[:4] == b'BFNP', 'not a postprocessed BFNP image: ' + role)
    app = safe_file(directory, 'application.bin').read_bytes()
    require(version.encode() + b'\0' in app, 'firmware version marker absent from application')
    actual = {p.relative_to(directory).as_posix() for p in directory.rglob('*') if p.is_file()}
    require(actual == set(listed) | {'release.json'}, 'unlisted files in release directory')
    return manifest


def git_state(path):
    return {'head':subprocess.check_output(['git','-C',str(path),'rev-parse','HEAD'],text=True).strip(),
            'dirty':bool(subprocess.check_output(['git','-C',str(path),'status','--porcelain'],text=True))}


def pack(build, output, version):
    require(not output.exists(), 'output already exists; never overwrite a release')
    require(re.fullmatch(r'\d+\.\d+\.\d+', version), 'invalid version')
    output.mkdir(parents=True)
    for role, (name, _, _) in IMAGES.items():
        suffix = {'boot2':'boot2.bin','partition':'partition.bin','application':'bin'}[role]
        shutil.copy2(build / ('UNOR4BL616CLBridge.ino.' + suffix), output / name)
    # Public distribution is an allowlist. Keep debug/build evidence local.
    for name in ['source_manifest.json', 'proprietary_manifest.json']:
        shutil.copy2(ROOT/'tools/bridge_runtime'/name, output/name.replace('_','-'))
    shutil.copytree(ROOT/'licenses/bl616cl', output/'licenses/bl616cl')
    for source in ['bl616cl_release.py','update_bl616cl_release.py']:
        shutil.copy2(ROOT/'tools'/source, output/source)
    shutil.copy2(ROOT/'docs/BL616CL-BRIDGE.md', output/'README.md')
    platform = ROOT
    manifest = {'schema':1,'chip':'bl616cl','board':BOARD,'layout':'bridge-v1-4MiB',
                'target_serial':SERIAL,'usb':{'vid':0x2341,'pid':0x1002,'interface':0},
                'firmware_version':version,'channel':'local-development',
                'source':{'bridge':git_state(ROOT),'platform':git_state(platform)},
                'images':{role:{'file':name,'address':address} for role,(name,address,_) in IMAGES.items()},
                'files':{p.relative_to(output).as_posix():{'bytes':p.stat().st_size,'sha256':digest(p.read_bytes())}
                         for p in sorted(output.rglob('*')) if p.is_file()}}
    (output/'release.json').write_text(json.dumps(manifest,indent=2)+'\n')
    verify(output)
    print('BL616CL_RELEASE_PACK_PASS', output)
    print('RELEASE_MANIFEST_SHA256='+digest((output/'release.json').read_bytes()))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='command',required=True)
    p=sub.add_parser('pack');p.add_argument('--build',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--version',required=True)
    p=sub.add_parser('verify');p.add_argument('directory',type=Path)
    args=parser.parse_args()
    if args.command=='pack':pack(args.build,args.output,args.version)
    else:
        m=verify(args.directory)
        print('BL616CL_RELEASE_VERIFY_PASS version='+m['firmware_version'])

if __name__=='__main__':
    try:main()
    except (ValueError,OSError,KeyError,TypeError) as error:raise SystemExit('REJECTED: '+str(error))
