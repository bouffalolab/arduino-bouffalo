"""STA OTA coverage using the already-running authorized ESP AP fixture."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from bl616cl_prepared_usb_session import PreparedUsbSession
from bl616cl_softap_test_session import SoftApTestSession
from bl616cl_usb_mapping import ensure_bl_usb_mapping
import run_ra4_ota_download_final_carrier as ota


def run_sta_ota(args, ssid, password, gateway):
    root=Path(__file__).resolve().parents[1]
    work=Path(tempfile.mkdtemp(prefix='controlled-ap-ota-',dir=root/'build'))
    session=SoftApTestSession(work)
    usb=PreparedUsbSession()
    changed=False
    primary=None
    try:
        usb.detach();changed=True
        session.prepare_association(args,ssid,password,gateway)
        session.connect_prepared()
        args.bridge_port=ensure_bl_usb_mapping(usb)
        host=ota.windows_wlan_address()
        env={**os.environ,'BRIDGE_TEST_STA_SSID':ssid,'BRIDGE_TEST_STA_PASSWORD':password,'PYTHONDONTWRITEBYTECODE':'1'}
        for label,extra in [('HTTPS',['--https']),('HTTP',['--synchronous'])]:
            log=work/(label.lower()+'.log')
            with log.open('w') as output:
                subprocess.run([sys.executable,str(root/'tools/run_ra4_ota_download_final_carrier.py'),'--session-sta-env','--peer-host',str(host),*extra],cwd=root,env=env,stdout=output,stderr=subprocess.STDOUT,check=True,timeout=240)
            print('CONTROLLED_STA_OTA_'+label+'_PASS log='+str(log.relative_to(root)),flush=True)
    except Exception as error:
        primary=error
    finally:
        # The child restores scan; the owning AP runner needs its proxy for AT cleanup.
        try:
            args.bridge_port=ensure_bl_usb_mapping(usb)
            opts=argparse.Namespace(bridge_port=args.bridge_port,arduino_cli=args.arduino_cli,runtime_settle_ms=1000)
            ota.upload_proxy(opts,ota.compile_proxy(opts))
        except Exception as error:
            if primary is None:primary=error
        try:
            if changed:usb.detach()
            session.close()
        except Exception as error:
            if primary is None:primary=error
        finally:
            try:
                if changed:args.bridge_port=ensure_bl_usb_mapping(usb)
            finally:usb.close()
    if primary is not None:raise primary
