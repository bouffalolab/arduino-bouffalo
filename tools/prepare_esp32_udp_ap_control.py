#!/usr/bin/env python3
"""Prepare an isolated ESP32-S3 AP for large UDP forwarding tests; never flash.

The original bridge source and installed WiFi library remain unchanged. Match
--partition-scheme to a verified Flash backup before using the built application.
"""
import os
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
# Read-only external baseline input; all generated copies and logs belong to ROOT/build.
REFERENCE = Path(os.environ.get('UNO_R4_REFERENCE_ROOT',str(ROOT.parent/'uno-r4-wifi-usb-bridge')))


def prepare(output, rx_buffers, sdk_static=True):
    if not 4 <= rx_buffers <= 16:
        raise ValueError('RX buffers must be between 4 and 16')
    if sdk_static and rx_buffers != 4:
        raise ValueError('Custom RX counts apply only to the dynamic profile')
    output = Path(output).resolve()
    if output == ROOT or any(path == output or path in output.parents for path in
                             (REFERENCE/'UNOR4USBBridge',REFERENCE/'hardware')):
        raise ValueError('Output must be separate from baseline source and platform')
    output.mkdir(parents=True, exist_ok=False)
    sketch = output / 'UNOR4USBBridge'
    sketch.mkdir()
    hashes = {}
    for source in sorted((REFERENCE / 'UNOR4USBBridge').iterdir()):
        if source.is_file() and source.suffix in ('.ino','.cpp','.c','.h','.hpp'):
            shutil.copy2(source,sketch/source.name)
            hashes['UNOR4USBBridge/'+source.name] = hashlib.sha256(source.read_bytes()).hexdigest()
    patch = ROOT / 'tools/esp32_udp_native_diagnostic.patch'
    subprocess.run(['patch','--batch','--fuzz=0','-p1','-i',str(patch)],cwd=sketch,check=True)
    if sdk_static:
        main=sketch/'UNOR4USBBridge.ino'
        text=main.read_text()
        before='  /* Set up wifi event */'
        if text.count(before)!=1:
            raise RuntimeError('Expected WiFi setup changed; review source')
        main.write_text(text.replace(before,'  WiFi.useStaticBuffers(true);\n'+before))
    library = REFERENCE / 'hardware/esp32-patched/esp32/libraries/WiFi'
    shutil.copytree(library,output/'WiFi')
    generic = output / 'WiFi/src/WiFiGeneric.cpp'
    text = generic.read_text()
    before = 'cfg.static_rx_buf_num = 4;'
    if text.count(before) != 1:
        raise RuntimeError('Expected Arduino dynamic buffer override changed; review source')
    generic.write_text(text.replace(before,f'cfg.static_rx_buf_num = {rx_buffers};'))
    for source in sorted(library.rglob('*')):
        if source.is_file():
            hashes['WiFi/'+str(source.relative_to(library))] = hashlib.sha256(source.read_bytes()).hexdigest()
    (output/'source-manifest.json').write_text(json.dumps({
        'purpose':'controlled AP UDP diagnostic, not baseline bridge firmware',
        'profile':'sdk-static' if sdk_static else 'arduino-dynamic',
        'dynamic_profile_static_rx_buffers':rx_buffers,'dynamic_profile_tx_buffers':32,
        'source_sha256':hashes,'native_patch_sha256':hashlib.sha256(patch.read_bytes()).hexdigest(),
    },indent=2)+'\n')
    return sketch


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--rx-buffers',type=int,default=4,help='Diagnostic RX override in the Arduino dynamic profile; raising it alone did not fix fragment loss')
    buffers=parser.add_mutually_exclusive_group()
    buffers.add_argument('--sdk-static-buffers',dest='sdk_static_buffers',action='store_true',help='Use SDK static RX/TX settings for reliable fragment forwarding (default)')
    buffers.add_argument('--arduino-dynamic-buffers',dest='sdk_static_buffers',action='store_false',help='Reproduce the original AP fragment-forwarding failure')
    parser.set_defaults(sdk_static_buffers=True)
    parser.add_argument('--partition-scheme',choices=('huge_app','unor4wifi'),required=True)
    parser.add_argument('--compile',action='store_true')
    parser.add_argument('--arduino-cli',default='arduino-cli')
    args=parser.parse_args()
    sketch=prepare(args.output,args.rx_buffers,args.sdk_static_buffers)
    if args.compile:
        fqbn='esp32-patched:esp32:arduino_unor4wifi_usb_bridge:USBMode=default,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,PartitionScheme='+args.partition_scheme
        output_config=sketch.parent/'arduino-cli.yaml'
        output_config.write_text('directories:\n  user: '+str(REFERENCE)+'\n')
        subprocess.run([args.arduino_cli,'compile','--config-file',str(output_config),
                        '--fqbn',fqbn,'--library',str(sketch.parent/'WiFi'),
                        '--build-path',str(sketch.parent/'compiled'),str(sketch)],cwd=ROOT,check=True)
    print('ESP32_UDP_AP_CONTROL_PREPARED profile='+('sdk-static' if args.sdk_static_buffers else 'arduino-dynamic')+' dynamic_profile_rx_buffers='+str(args.rx_buffers))


if __name__=='__main__':
    main()
