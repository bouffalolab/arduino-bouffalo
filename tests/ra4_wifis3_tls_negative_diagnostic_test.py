#!/usr/bin/env python3
"""Reject pool/socket failures as evidence of negative CA verification."""
from pathlib import Path
import subprocess
import tempfile
from bl616cl_sta_disconnect_wait_test import function
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'examples/ra4-wifis3-tls-resource-gate/ra4-wifis3-tls-resource-gate.ino.in').read_text()
body=function(source,'bool query_tls_failure_diagnostic(int socket)')
prefix=r'''
#include <cassert>
#include <cstdlib>
#include <string>
#include <vector>
struct Modem {
 std::vector<std::string> replies;size_t used=0;
 void begin() {}
 bool write(const char *,std::string &out,const char *,int) {
  if(used>=replies.size())return false;
  out=replies[used++];return true;
 }
} modem;
'''
main=r'''
int main() {
 modem.replies={"21,-105,0"};modem.used=0;
 assert(!query_tls_failure_diagnostic(2003) && modem.used==1);
 modem.replies={"4,-9984,0","8"};modem.used=0;
 assert(query_tls_failure_diagnostic(2003) && modem.used==2);
 modem.replies={"4,-9984,0","0"};modem.used=0;
 assert(!query_tls_failure_diagnostic(2003));
 modem.replies={"4,-9984,0"};modem.used=0;
 assert(!query_tls_failure_diagnostic(2003));
 modem.replies={"6,0,0","8"};modem.used=0;
 assert(!query_tls_failure_diagnostic(2003) && modem.used==1);
 modem.replies={"malformed"};modem.used=0;
 assert(!query_tls_failure_diagnostic(2003));
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'negative.cpp').write_text(prefix+body+main)
 subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror',str(p/'negative.cpp'),'-o',str(p/'negative')],check=True)
 subprocess.run([str(p/'negative')],check=True)
print('RA4_WIFIS3_TLS_NEGATIVE_DIAGNOSTIC_PASS')
