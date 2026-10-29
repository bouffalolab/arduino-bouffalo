#!/usr/bin/env python3
"""Run new-image hardware gates sequentially and preserve failures and restoration logs."""
import argparse,hashlib,json,os,subprocess,time
from pathlib import Path
root=Path(__file__).resolve().parents[2]; out=root/'build/migration-evidence'; out.mkdir(parents=True,exist_ok=True)
image_sha=hashlib.sha256((root/'build/bridge-image/UNOR4BL616CLBridge.ino.bin').read_bytes()).hexdigest()
# Identity verification precedes every gate; each runner also checks before writes.
gates=[
 ('scan',['run_ra4_wifis3_scan_final_carrier.py']),
 ('generic-at',['run_bl616cl_generic_at_final_carrier.py']),
 ('resource',['run_ra4_wifis3_tls_resource_gate.py','--softap','--direct-ip','--rounds','2','--receive-burst-bytes','4096','--ble-scan','--usb-echo','--mixed-probe','--mixed-udp-send','--ra4-sized-response-reserve','--skip-peer-firewall']),
 ('storage',['run_ra4_storage_final_carrier.py','--windows-ftdi-reset']),
 ('softse',['run_ra4_softse_final_carrier.py','--windows-ftdi-reset']),
 ('ble-gatt',['run_ra4_ble_windows_long_gatt_gate.py','--connected-coex','--peer-log',str(out/'ble-gatt-peer.log')]),
 ('ble-notify',['run_ra4_ble_windows_long_gatt_gate.py','--mode','notify','--peer-log',str(out/'ble-notify-peer.log')]),
 ('ble-indicate',['run_ra4_ble_windows_long_gatt_gate.py','--mode','indicate','--peer-log',str(out/'ble-indicate-peer.log')]),
 ('ble-long-notify',['run_ra4_ble_windows_long_gatt_gate.py','--mode','long-notify','--peer-log',str(out/'ble-long-notify-peer.log')]),
 ('ota-update',['run_ra4_ota_update_final_carrier.py','--softap']),
 ('ota-https',['run_ra4_ota_download_final_carrier.py','--https','--softap']),
 ('ota-failure',['run_ra4_ota_failure_final_carrier.py','--softap']),
 ('swd-pyocd',['run_bl616cl_swd_d5_pyocd.py']),
 ('swd-openocd',['run_bl616cl_swd_d5_openocd.py']),
 ('swd-program',['run_bl616cl_swd_d7_flash_program.py']),
]
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--only',nargs='+',choices=[g[0] for g in gates]);selected=parser.parse_args().only
results=json.loads((out/'hardware-results.json').read_text()) if (out/'hardware-results.json').exists() else [];env=os.environ.copy(); env['ARDUINO_CONFIG_FILE']=str(root/'build/migration-workspace/arduino-cli.yaml'); env['PYTHONDONTWRITEBYTECODE']='1'
for name,args in gates:
 if selected and name not in selected:continue
 if any(r['gate']==name and r['exit_code']==0 and r.get('application_sha256')==image_sha for r in results):continue
 log=out/(name+'-'+str(time.time_ns())+'.log'); start=time.monotonic()
 with log.open('w') as f:
  # Approved WSL root surface for live enumeration, then strict chip/port open in runner.
  try:
   if name=='swd-program':
    subprocess.run(['arduino-cli','compile','--fqbn','arduino:renesas_uno:unor4wifi','--build-path',str(root/'build/ra4-swd-sram-scratch'),str(root/'examples/ra4-swd-sram-scratch')],cwd=root,env=env,stdout=f,stderr=subprocess.STDOUT,check=True,timeout=180)
    subprocess.run(['python3',str(root/'tools/ra4_swd_flash_loader/build_ra4_swd_flash_loader.py')],cwd=root,env=env,stdout=f,stderr=subprocess.STDOUT,check=True,timeout=180)
   command=['python3','-u',str(root/'tools'/args[0]),*args[1:]]
   if name in ('ota-https','ota-failure') and '--softap' not in args:command=['python3',str(root/'tools/migration/ensure_sta_route.py'),*command]
   code=subprocess.run(command,cwd=root,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=1200).returncode
  except subprocess.TimeoutExpired:code=124
  except subprocess.CalledProcessError as error:code=error.returncode
 results.append({'gate':name,'application_sha256':image_sha,'exit_code':code,'seconds':round(time.monotonic()-start,1),'log':str(log.relative_to(root))})
 (out/'hardware-results.json').write_text(json.dumps(results,indent=2)+'\n')
 print(name,code,flush=True)
 if code:break
