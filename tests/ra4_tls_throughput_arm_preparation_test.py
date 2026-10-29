"""Execute throughput's real AP preparation branch and fixture command parser."""
import ast
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import Mock,patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import run_ra4_wifis3_tls_throughput_gate as gate
from bl616cl_pbuf_release_lifecycle_test import function

class PreparationTest(unittest.TestCase):
    def test_real_ap_branch_orders_arm_detach_settle_connect_reopen(self):
        tree=ast.parse(Path(gate.__file__).read_text())
        branch=next(n for n in ast.walk(tree) if isinstance(n,ast.If) and any(
            isinstance(s,ast.ImportFrom) and s.module=='bl616cl_softap_test_session' for s in n.body))
        code=compile(ast.fix_missing_locations(ast.Module(body=branch.body,type_ignores=[])),gate.__file__,'exec')
        for fail_arm in (False,True):
            actions=[];old=Mock();session=Mock();new=Mock();helper=object()
            session.prepare_association.side_effect=lambda *a:actions.append('profile')
            session.connect_prepared.side_effect=lambda:actions.append('connect')
            session.current_address.return_value='192.168.4.100'
            old.write.side_effect=lambda data:(actions.append('arm'),len(data))[1] if data.startswith(b'ARMPAP ') else self.fail('wrong command')
            old.out_waiting=0
            def wait(*args):
                self.assertEqual(args[1:3],('RA4_TLS_THROUGHPUT:SOFTAP_ARMED',5))
                if fail_arm:raise RuntimeError('arm missing')
                actions.append('confirmed')
            def detach(args,bridge,root):
                self.assertIs(bridge,old);self.assertIs(root,helper);actions.append('detach')
            def reopen(link):
                self.assertIsNone(link);actions.append('reopen');return new
            ns=dict(vars(gate),bridge=old,args=SimpleNamespace(),ssid='test',password='temporary',host=gate.ipaddress.IPv4Address('192.168.4.100'),work_dir=Path('/unused'),detach_helper=helper,wait_marker=wait,reopen_after_association=reopen,query_preparation=lambda link:actions.append('query') if link is new else self.fail('wrong probe port'))
            with patch('bl616cl_softap_test_session.SoftApTestSession',return_value=session),patch('run_ra4_wifis3_tls_resource_gate.suspend_before_association',side_effect=detach),patch.object(gate.time,'sleep',side_effect=lambda seconds:actions.append(('settle',seconds))):
                if fail_arm:
                    with self.assertRaises(RuntimeError):exec(code,ns)
                    self.assertEqual(actions,['profile','arm']);session.connect_prepared.assert_not_called()
                else:
                    exec(code,ns)
                    self.assertEqual(actions,['profile','arm','confirmed','detach',('settle',10),'connect','reopen',('settle',1),'query'])
                    self.assertIs(ns['bridge'],new)

    def test_host_probe_rejects_stale_duplicate_and_short_reply(self):
        nonce='abcdef0123456789';marker='RA4_TLS_THROUGHPUT:PREPSTATUS='+nonce+',1'
        for reply,valid in ((marker+'\n',True),(marker+'\n'+marker+'\n',False),('RA4_TLS_THROUGHPUT:PREPSTATUS=0000000000000000,1\n',False),(marker[:-1]+'0\n',False)):
            link=Mock();link.write.side_effect=lambda data:len(data);link.out_waiting=0
            with patch.object(gate.secrets,'token_hex',return_value=nonce),patch.object(gate,'wait_marker',return_value=reply):
                if valid:gate.query_preparation(link)
                else:
                    with self.assertRaises(RuntimeError):gate.query_preparation(link)
                link.write.assert_called_once_with(('@PREPSTATUS '+nonce+'\n').encode('ascii'))
        link=Mock();link.write.return_value=1
        with self.assertRaises(gate.serial.SerialTimeoutException):gate.query_preparation(link)
        link.flush.assert_not_called()

    def test_native_fixture_parser_sta_and_ap_commands(self):
        source=gate.TEMPLATE.read_text()
        native='\n'.join(function(source,signature) for signature in ('int hex_digit(char value)','bool decode_hex(char *field)','bool parse_start(char *&ssid, char *&password, IPAddress &peer, uint16_t &port)'))
        loop=function(source,'void loop()')
        harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static constexpr bool kSoftAp=AP;
static char command[512];
struct IPAddress { bool fromString(const char *s) { return !strcmp(s,"192.168.4.100"); } };
'''+native+r'''
int main() {
 for(const char *prefix: {"START ","PREPAP ","ARMPAP "}) {
  strcpy(command,prefix);strcat(command,"74657374 70617373 192.168.4.100 18443");
  char *ssid=nullptr,*password=nullptr;IPAddress peer;uint16_t port=0;
  bool accepted=parse_start(ssid,password,peer,port);
  assert(accepted==(kSoftAp || !strcmp(prefix,"START ")));
  if(accepted) assert(!strcmp(ssid,"test") && !strcmp(password,"pass") && port==18443);
 }
}
'''
        harness += r'''
#include <string>
static bool finished=false,softap_prepared=true,preparation_queried=false;
static unsigned command_size=0,run_calls=0;
static unsigned long millis(){return 0;}
static void delay(unsigned){}
static void run_gate(){++run_calls;}
static void fail(const char*){assert(false);}
struct SerialFake {
 std::string input,output;
 int available(){return !input.empty();}
 int read(){char c=input.front();input.erase(0,1);return c;}
 void print(const char *s){output+=s;}
 void println(const char *s){output+=s;output+='\n';}
} Serial;
''' + loop + r'''
static void loop_probe_test() {
 command_size=0;preparation_queried=false;run_calls=0;
 Serial.input="leftover@PREPSTATUS abcdef0123456789\n";Serial.output.clear();
 loop();
 if(kSoftAp) {
  assert(preparation_queried && !run_calls);
  assert(Serial.output=="RA4_TLS_THROUGHPUT:PREPSTATUS=abcdef0123456789,1\n");
  preparation_queried=false;Serial.input="@PREPSTATUS invalid\n";Serial.output.clear();loop();
  assert(!preparation_queried && Serial.output.empty() && !run_calls);
 } else {assert(!preparation_queried && run_calls==1);}
}
'''
        harness=harness.replace('int main() {','static void loop_probe_test();\nint main() {\n loop_probe_test();',1)
        with tempfile.TemporaryDirectory() as d:
            src=Path(d)/'test.cpp';src.write_text('#include <initializer_list>\n'+harness)
            for ap in (0,1):
                binary=Path(d)/'test'
                subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',f'-DAP={ap}',str(src),'-o',str(binary)],check=True)
                subprocess.run([str(binary)],check=True)

if __name__=='__main__':unittest.main()
