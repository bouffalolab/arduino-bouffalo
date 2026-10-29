"""Execute production TLS availability with a saved probe byte and record tail."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AvailableBytesTest(unittest.TestCase):
    def test_saved_byte_and_record_tail(self):
        text = (ROOT/'UNOR4BL616CLBridge/bouffalo_tls_client_backend.cpp').read_text()
        start = text.index('bool available_tls(')
        end = text.index('\nbool peek_tls(', start)
        source = r'''
#include <cassert>
#include <cstddef>
struct TlsConnection { bool has_peeked; size_t ssl; } connection;
static bool alive=true, probe_ok=true;
static unsigned probes;
static TlsConnection *get_connection(int handle) { return handle==0 ? &connection : nullptr; }
static bool probe_tls_peer(TlsConnection &,bool *connected) { ++probes; *connected=alive;return probe_ok; }
static size_t mbedtls_ssl_get_bytes_avail(const size_t *ssl) { return *ssl; }
''' + text[start:end] + r'''
int main() {
 size_t result=999;
 assert(!available_tls(nullptr,1,&result) && probes==0);
 assert(!available_tls(nullptr,0,nullptr) && probes==0);
 for(size_t tail: {size_t(0),size_t(1),size_t(1023),size_t(16384)}) {
  for(bool saved: {false,true}) {
   connection={saved,tail};
   assert(available_tls(nullptr,0,&result) && result==tail+(saved?1U:0U));
   assert(connection.has_peeked==saved && connection.ssl==tail);
  }
 }
 alive=false;connection={true,1023};
 assert(available_tls(nullptr,0,&result) && result==0);
 probe_ok=false;assert(!available_tls(nullptr,0,&result));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'test.cpp'
            path.write_text('#include <initializer_list>\n'+source)
            binary = Path(directory)/'test'
            subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined',str(path),'-o',str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
