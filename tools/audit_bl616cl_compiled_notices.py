#!/usr/bin/env python3
"""Preserve CMake runtime compilation-unit hashes and leading source notices.

Use the generator's --keep-build output. This inventories built source units,
not final linker reachability or a complete legal review.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path


def audit(build, sdk_overlay, output):
    if output.exists():
        raise ValueError('audit output exists')
    records = {}
    headers = []
    for dep in sorted(build.rglob('DependInfo.cmake')):
        for name in re.findall(r'^\s*"([^"\n]+)"\s+"[^"\n]+"\s+"gcc"', dep.read_text(), re.M):
            path = Path(name)
            if not path.is_file():
                raise ValueError('retained build source missing: '+name)
            key = 'sdk/'+path.relative_to(sdk_overlay).as_posix() if path.is_relative_to(sdk_overlay) else 'profile/'+path.name
            data = path.read_bytes()
            text = data.decode(errors='replace')
            match = re.match(r'\s*((?:(?:/\*.*?\*/|//[^\n]*(?:\n|$))\s*)+)', text, re.S)
            header = match[1] if match else ''
            records[key] = {'sha256': hashlib.sha256(data).hexdigest(),
                            'spdx': re.findall(r'SPDX-License-Identifier:\s*([^\n*]+)', header),
                            'header_present': bool(header)}
            if header:
                headers.append('\n===== '+key+' =====\n'+header+'\n')
    if not records:
        raise ValueError('no CMake compilation units found; keep the actual build directory')
    output.mkdir(parents=True)
    (output/'compiled-sources.json').write_text(json.dumps({
        'schema': 1, 'scope': 'runtime CMake compilation units, not final linker reachability',
        'files': dict(sorted(records.items())),
        'notice': 'Original headers are preserved as evidence; absence of SPDX does not assign or negate a license.'}, indent=2)+'\n')
    (output/'COMPILED_SOURCE_HEADERS.txt').write_text(''.join(headers))
    print('BL616CL_COMPILED_NOTICE_AUDIT_PASS units='+str(len(records)))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--sdk-overlay', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    audit(args.build.resolve(), args.sdk_overlay.resolve(), args.output)
