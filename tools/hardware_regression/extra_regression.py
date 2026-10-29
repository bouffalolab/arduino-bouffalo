#!/usr/bin/env python3
"""Run FOTA roundtrip and reversible controlled-AP regression after the main gates."""
import argparse,hashlib,json,os,subprocess,sys,time
from pathlib import Path
T=Path(__file__).resolve().parents[2];ev=T/'build/migration-evidence';results=[]
if (ev/'extra-results.json').exists():results=json.loads((ev/'extra-results.json').read_text())
env={**os.environ,'PYTHONDONTWRITEBYTECODE':'1','ARDUINO_CONFIG_FILE':str(T/'build/migration-workspace/arduino-cli.yaml')}
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--only-ap',action='store_true');parser.add_argument('--fixture-ready',action='store_true');options=parser.parse_args()
if options.fixture_ready and not options.only_ap:parser.error('--fixture-ready requires --only-ap')
base=T/'build/migration-final-artifacts/UNOR4BL616CLBridge.ino.bin';alt=T/'build/fota-alt/UNOR4BL616CLBridge.ino.bin'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
if sha(base)!=sha(T/'build/bridge-image/UNOR4BL616CLBridge.ino.bin'):raise SystemExit('Frozen final application differs from current build')
main=json.loads((ev/'hardware-results.json').read_text());expected=['scan','generic-at','resource','storage','softse','ble-gatt','ble-notify','ble-indicate','ble-long-notify','ota-update','ota-https','ota-failure','swd-pyocd','swd-openocd','swd-program']
if any(not any(r['gate']==g and r['exit_code']==0 and r.get('application_sha256')==sha(base) for r in main) for g in expected):raise SystemExit('Complete main hardware regression before extra gates')
def run(name,args,timeout=1500,**metadata):
 log=ev/(name+'-'+str(time.time_ns())+'.log');started=time.monotonic()
 with log.open('w') as f:
  try:code=subprocess.run([sys.executable,*map(str,args)],cwd=T,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=timeout).returncode
  except subprocess.TimeoutExpired:code=124
 results.append({'gate':name,'exit_code':code,'seconds':round(time.monotonic()-started,1),'application_sha256':sha(base),'log':str(log.relative_to(T)),**metadata})
 (ev/'extra-results.json').write_text(json.dumps(results,indent=2)+'\n');print(name,code,flush=True)
 return code
for name,image,initial,version in ([] if options.only_ap else [('self-fota-forward',alt,'0.6.0','0.6.1'),('self-fota-back',base,'0.6.1','0.6.0')]):
 if run(name,['tools/run_bl616cl_self_fota_gate.py','--application',image,'--expected-version',version,'--initial-version',initial,'--check-header'],installed_application_sha256=sha(image)):
  run('self-fota-failure-restore',['tools/hardware_regression/flash_target.py'])
  raise SystemExit('FOTA gate failed; base restoration attempted; inspect preserved logs')
try:
 if options.fixture_ready and not (T/'build/esp32-baseline-private/manifest.json').is_file():raise RuntimeError('Require existing verified private fixture backup')
 if not options.fixture_ready and run('esp-ap-fixture-install',['tools/hardware_regression/esp_fixture.py','install']):raise RuntimeError('AP installation failed')
 if run('controlled-ap-32x16k',['tools/run_bl616cl_controlled_ap_recovery.py','--host-services-work-dir','build/migration-controlled-ap-host','--udp-large-count','32','--ap-tx-diagnostic','--sta-ota'],timeout=2400):raise RuntimeError('Controlled AP regression failed')
finally:
 # Full backup remains private, never include it in a public evidence archive.
 if (T/'build/esp32-baseline-private/manifest.json').exists():
  if run('esp-baseline-flash-restore',['tools/hardware_regression/esp_fixture.py','restore']):raise RuntimeError('ESP full Flash restoration failed')
  if run('esp-baseline-scan-restore',['-c',"import sys;sys.path.insert(0,'tools');from ble_independent_failure_probe import prepare_baseline_usb,restore_probe;from uno_r4_usb_identity import resolve_baseline_esp32_port;prepare_baseline_usb();restore_probe(resolve_baseline_esp32_port('auto'))"]):raise RuntimeError('ESP RA4 scan restoration failed')
 if run('final-bl-scan',['tools/run_ra4_wifis3_scan_final_carrier.py']):raise RuntimeError('BL RA4 scan restoration failed')
print('EXTRA_REGRESSION_AND_RESTORATION_PASS',flush=True)
