#!/usr/bin/env python3
"""Build the isolated UNO R4 runtime exclusively from this repository's locked sources."""
import argparse,hashlib,json,os,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser(); p.add_argument('--platform-root',type=Path,required=True); p.add_argument('--force',action='store_true'); a=p.parse_args()
root=a.platform_root.resolve(); lock=json.loads((root/'third_party/bouffalo_sdk.lock.json').read_text()); sdk=root/lock['path']
def git(path,*args): return subprocess.check_output(['git','-C',str(path),*args],text=True).strip()
for path,commit in [(sdk,lock['commit']),*[(sdk/n,e['commit']) for n,e in lock['required_submodules'].items()]]:
 if git(path,'rev-parse','HEAD')!=commit or git(path,'status','--porcelain','--untracked-files=no'):
  raise SystemExit(f'Bridge SDK input does not match clean source lock: {path}')
profile=root/'tools/bridge_runtime'; tc=root/'tools/Xuantie-900-gcc'
digest=hashlib.sha256()
for x in sorted(profile.rglob('*')):
 if x.is_file() and '__pycache__' not in x.parts and 'build' not in x.relative_to(profile).parts:
  digest.update(str(x.relative_to(profile)).encode()); digest.update(x.read_bytes())
digest.update(json.dumps(lock,sort_keys=True).encode()); digest.update(Path(__file__).read_bytes())
digest.update(subprocess.check_output([str(tc/'bin/riscv64-unknown-elf-gcc'),'--version']))
key=digest.hexdigest(); stage=root/'build/bridge-runtime'; marker=stage/'source-key.json'; runtime=stage/'tools/sdk/bl616cl/bridge'
if a.force or not marker.exists() or json.loads(marker.read_text())['key']!=key:
 if stage.exists(): shutil.rmtree(stage)
 template=stage/'tools/runtime_bundle'; shutil.copytree(profile,template,ignore=shutil.ignore_patterns('__pycache__','build'))
 subprocess.run(['python3',str(template/'generate_runtime_bundle.py'),'--chip','bl616cl','--profile','bridge','--sdk',str(sdk),'--toolchain',str(tc)],check=True)
 marker.write_text(json.dumps({'key':key,'sdk':str(sdk),'commit':lock['commit']},indent=2)+'\n')
 # Host executables are supplier tools, not runtime archive inputs.
 for name in ['bflb_fw_post_proc','bouffalo_flash_cube']:
  shutil.copytree(stage/'tools'/name,root/'tools'/name,dirs_exist_ok=True)
 subprocess.run(['python3',str(template/'verify_runtime_bundle.py'),'--help'],check=True,stdout=subprocess.DEVNULL)
print('BRIDGE_SOURCE_RUNTIME',runtime,'key='+key,flush=True)
