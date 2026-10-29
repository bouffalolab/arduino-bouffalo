#!/usr/bin/env python3
"""Run new-image hardware gates sequentially and preserve failures and restoration logs."""
import json,os,subprocess,time
from pathlib import Path
root=Path(__file__).resolve().parents[2]; out=root/'build/migration-evidence'; out.mkdir(parents=True,exist_ok=True)
# Identity verification precedes every gate; each runner also checks before writes.
gates=[
 ('scan',['run_ra4_wifis3_scan_final_carrier.py']),
 ('generic-at',['run_bl616cl_generic_at_final_carrier.py']),
 ('resource',['run_ra4_wifis3_tls_resource_gate.py','--softap','--direct-ip','--rounds','2','--receive-burst-bytes','4096','--ble-scan','--usb-echo','--mixed-probe','--mixed-udp-send','--ra4-sized-response-reserve','--skip-peer-firewall']),
 ('storage',['run_ra4_storage_final_carrier.py']),
 ('softse',['run_ra4_softse_final_carrier.py']),
 ('ble-gatt',['run_ra4_ble_windows_long_gatt_gate.py','--connected-coex','--peer-log',str(out/'ble-gatt-peer.log')]),
 ('ble-notify',['run_ra4_ble_windows_long_gatt_gate.py','--mode','notify','--peer-log',str(out/'ble-notify-peer.log')]),
 ('ble-indicate',['run_ra4_ble_windows_long_gatt_gate.py','--mode','indicate','--peer-log',str(out/'ble-indicate-peer.log')]),
 ('ble-long-notify',['run_ra4_ble_windows_long_gatt_gate.py','--mode','long-notify','--peer-log',str(out/'ble-long-notify-peer.log')]),
 ('ota-update',['run_ra4_ota_update_final_carrier.py','--softap']),
 ('ota-https',['run_ra4_ota_download_final_carrier.py','--https']),
 ('ota-failure',['run_ra4_ota_failure_final_carrier.py']),
 ('swd-pyocd',['run_bl616cl_swd_d5_pyocd.py']),
 ('swd-openocd',['run_bl616cl_swd_d5_openocd.py']),
 ('swd-program',['run_bl616cl_swd_d7_flash_program.py']),
]
results=json.loads((out/'hardware-results.json').read_text()) if (out/'hardware-results.json').exists() else [];env=os.environ.copy(); env['ARDUINO_CONFIG_FILE']=str(root/'build/migration-workspace/arduino-cli.yaml'); env['PYTHONDONTWRITEBYTECODE']='1'
for name,args in gates:
 if any(r['gate']==name and r['exit_code']==0 for r in results):continue
 log=out/(name+'-'+str(time.time_ns())+'.log'); start=time.monotonic()
 with log.open('w') as f:
  # Approved WSL root surface for live enumeration, then strict chip/port open in runner.
  try:code=subprocess.run(['python3','-u',str(root/'tools'/args[0]),*args[1:]],cwd=root,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=1200).returncode
  except subprocess.TimeoutExpired:code=124
 results.append({'gate':name,'exit_code':code,'seconds':round(time.monotonic()-start,1),'log':str(log.relative_to(root))})
 (out/'hardware-results.json').write_text(json.dumps(results,indent=2)+'\n')
 print(name,code,flush=True)
 if code:break
