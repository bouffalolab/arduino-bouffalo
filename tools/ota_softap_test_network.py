"""Own an isolated OTA peer network, including reassociation after BL reset."""
import ipaddress,secrets,time
from bl616cl_softap_test_session import SoftApTestSession
class OtaSoftApNetwork:
 def __init__(self,args,work):
  self.args=args;self.work=work;self.ssid='uno-r4-'+secrets.token_hex(4);self.password=secrets.token_urlsafe(18);self.session=None;self.associations=0
  self.host=ipaddress.IPv4Address('192.168.4.100')
 def start(self,bridge,timeout):
  import run_ra4_ota_download_final_carrier as d
  def exact(command,response):d.require_exact(d.transact(bridge,command,timeout),response,'OTA isolated network preparation')
  exact(b'AT+WIFIMODE=2\r\n',b'+WIFIMODE: \r\nOK\r\n')
  exact(b'AT+RECONNECT=0\r\n',b'OK\r\n')
  exact(b'AT+SOFTAPCONFIG=192.168.4.1,192.168.4.1,255.255.255.0\r\n',b'+SOFTAPCONFIG: \r\nOK\r\n')
  first=self.session is None
  if first:
   self.associations+=1
   directory=self.work/('wlan-session-'+str(self.associations));directory.mkdir()
   self.session=SoftApTestSession(directory)
   self.session.prepare_association(self.args,self.ssid,self.password,'192.168.4.1')
  request=('AT+BEGINSOFTAP='+d.quote_at_argument(self.ssid)+','+d.quote_at_argument(self.password)+',1\r\n').encode()
  exact(request,b'+BEGINSOFTAP: 1\r\nOK\r\n')
  deadline=time.monotonic()+45
  while d.parse_status(d.transact(bridge,b'AT+GETSTATUS?\r\n',timeout)) not in (7,8):
   if time.monotonic()>deadline:raise RuntimeError('OTA isolated AP did not become ready')
   time.sleep(.25)
  if first:self.session.connect_prepared()
  else:self.session.request_preparation_rejoin()
  if ipaddress.IPv4Address(self.session.current_address())!=self.host:raise RuntimeError('OTA isolated peer address differs from listener')
  print('RA4_OTA_ISOLATED_SOFTAP_CONNECT_PASS',flush=True)
 def close(self):
  if self.session:self.session.close()

 def pause(self):
  self.close();self.session=None
