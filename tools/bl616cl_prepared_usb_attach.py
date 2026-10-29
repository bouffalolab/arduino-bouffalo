"""Prepare the dedicated USB/IP attach process before a Windows WLAN change."""
from pathlib import Path
import select
import subprocess
from bl616cl_usb_mapping import dedicated_bus_id

ROOT_ATTACH_CODE = r'''
import ipaddress,re,subprocess,sys
sys.path.insert(0,sys.argv[1])
from bl616cl_usb_mapping import stale_bl_ports
routes=subprocess.run(['ip','-4','route','show','default'],capture_output=True,text=True,timeout=5,check=True).stdout
gateways=re.findall(r'^default via (\S+)',routes,re.MULTILINE)
if len(set(gateways))!=1:raise RuntimeError('no unique WSL host gateway')
gateway=str(ipaddress.IPv4Address(gateways[0]))
bus_id=sys.argv[2]
print('BL616CL_PREPARED_ATTACH_READY',flush=True)
if sys.stdin.readline().strip()!='ATTACH':sys.exit(0)
print('BL616CL_PREPARED_ATTACH_ENTER',flush=True)
for path,port in stale_bl_ports():path.write_text(str(port))
try:
 result=subprocess.run(['usbip','attach','-r',gateway,'-b',bus_id],capture_output=True,text=True,timeout=15)
 print(result.stdout,end='',flush=True)
 print(result.stderr,end='',file=sys.stderr,flush=True)
 if result.returncode:sys.exit(result.returncode)
except subprocess.TimeoutExpired:
 print('BL616CL_PREPARED_ATTACH_NATIVE_TIMEOUT',flush=True)
 sys.exit(1)
print('BL616CL_PREPARED_ATTACH_EXIT',flush=True)
'''


class PreparedBlAttach:
    def __init__(self):
        self.process = subprocess.Popen(
            ['wsl.exe', '-d', 'Ubuntu-22.04', '-u', 'root', '--', 'python3',
             '-u', '-c', ROOT_ATTACH_CODE, str(Path(__file__).resolve().parent), dedicated_bus_id()],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            if not select.select([self.process.stdout], [], [], 10)[0]:
                raise RuntimeError('prepared attach root readiness timeout')
            if self.process.stdout.readline().strip() != b'BL616CL_PREPARED_ATTACH_READY':
                raise RuntimeError('prepared attach root failed before ready')
        except Exception:
            self.close()
            raise
        print('BL616CL_PREPARED_ATTACH_READY', flush=True)

    def attach(self):
        try:
            output, error = self.process.communicate(input=b'ATTACH\n', timeout=20)
        except subprocess.TimeoutExpired:
            self.close()
            raise
        print(output.decode('ascii', errors='replace'), end='', flush=True)
        if error:
            print(error.decode('ascii', errors='replace'), end='', flush=True)
        if self.process.returncode or b'BL616CL_PREPARED_ATTACH_EXIT' not in output:
            raise RuntimeError('prepared native USB/IP attach failed')

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=5)
