"""One prepared root process for dedicated BL USB detach/attach cycles."""
from pathlib import Path
import select
import subprocess
from bl616cl_usb_mapping import dedicated_bus_id

ROOT_CODE = r'''
import ipaddress,re,subprocess,sys,time
sys.path.insert(0,sys.argv[1])
from bl616cl_usb_mapping import stale_bl_ports
def host_gateway(timeout=5):
 routes=subprocess.run(['ip','-4','route','show','default'],capture_output=True,text=True,timeout=timeout,check=True).stdout
 gateways=re.findall(r'^default via (\S+)',routes,re.MULTILINE)
 if len(set(gateways))!=1:raise RuntimeError('no unique WSL host gateway')
 return str(ipaddress.IPv4Address(gateways[0]))
host_gateway()
bus_id=sys.argv[2]
print('BL_USB_SESSION_READY',flush=True)
for line in sys.stdin:
 command=line.strip()
 if command=='EXIT':break
 try:
  if command not in ('ATTACH','DETACH'):raise RuntimeError('unsupported operation')
  for path,port in stale_bl_ports():path.write_text(str(port))
  if command=='ATTACH':
   deadline=time.monotonic()+15
   for attempt in range(2):
    remaining=deadline-time.monotonic()
    if remaining<=0:raise TimeoutError('native attach deadline')
    gateway=host_gateway(min(5,remaining))
    remaining=deadline-time.monotonic()
    if remaining<=0:raise TimeoutError('native attach deadline')
    result=subprocess.run(['usbip','attach','-r',gateway,'-b',bus_id],capture_output=True,timeout=remaining)
    if not result.returncode:break
    print(result.stderr.decode('ascii',errors='replace'),end='',file=sys.stderr,flush=True)
    if attempt or time.monotonic()+.25>=deadline:raise RuntimeError('native attach failed')
    print('BL_USB_SESSION_ATTACH_RETRY code='+str(result.returncode),flush=True)
    time.sleep(.25)
  print('BL_USB_SESSION_'+command+'_OK',flush=True)
 except Exception:
  print('BL_USB_SESSION_'+command+'_FAILED',flush=True)
'''


class PreparedUsbSession:
    def __init__(self):
        self.process = subprocess.Popen(['wsl.exe','-d','Ubuntu-22.04','-u','root','--',
            'python3','-u','-c',ROOT_CODE,str(Path(__file__).resolve().parent), dedicated_bus_id()],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self._expect(b'BL_USB_SESSION_READY',30)
        except Exception:
            self.close(); raise
        print('BL_USB_SESSION_READY',flush=True)

    def _expect(self, marker, timeout):
        import time
        deadline=time.monotonic()+timeout
        while True:
            if not select.select([self.process.stdout],[],[],max(0,deadline-time.monotonic()))[0]:
                raise RuntimeError('prepared USB session response deadline')
            line=self.process.stdout.readline().strip()
            if line.startswith(b'BL_USB_SESSION_ATTACH_RETRY code='):
                print(line.decode('ascii'),flush=True)
                continue
            if line!=marker:
                raise RuntimeError('prepared USB session operation failed')
            return

    def _command(self, command):
        self.process.stdin.write(command.encode('ascii')+b'\n'); self.process.stdin.flush()
        self._expect(('BL_USB_SESSION_'+command+'_OK').encode('ascii'),20)
        print('BL_USB_SESSION_'+command+'_OK',flush=True)

    def attach(self):self._command('ATTACH')
    def detach(self):self._command('DETACH')

    def close(self):
        if self.process.poll() is None:
            try:
                _,error=self.process.communicate(input=b'EXIT\n',timeout=5)
                if error:print(error.decode('ascii',errors='replace'),end='',flush=True)
            except subprocess.TimeoutExpired:
                self.process.terminate(); self.process.wait(timeout=5)
