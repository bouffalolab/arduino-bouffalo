"""Execute the production TLS receiver across record tails, WANT and closure."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'UNOR4BL616CLBridge/bouffalo_tls_client_backend.cpp').read_text()
function = source[source.index('bool receive_tls('):source.index('\nbool available_tls(')]
prefix = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
using std::size_t;
constexpr int MBEDTLS_ERR_SSL_WANT_READ=-1, MBEDTLS_ERR_SSL_WANT_WRITE=-2;
struct Ssl { std::vector<int> records; size_t offset=0; unsigned calls=0; };
struct TlsConnection { bool connected=true,has_peeked=false; uint8_t peeked=0; Ssl ssl; } connection;
TlsConnection *get_connection(int handle) {return handle==0 ? &connection : nullptr;}
void mark_tls_peer_disconnected(TlsConnection &c,int) {c.connected=false;}
int mbedtls_ssl_read(Ssl *ssl,uint8_t *out,size_t capacity) {
 assert(++ssl->calls<100); assert(capacity>0);
 if(ssl->records.empty()) return MBEDTLS_ERR_SSL_WANT_READ;
 int record=ssl->records.front();
 if(record<=0) {ssl->records.erase(ssl->records.begin());return record;}
 size_t count=std::min(capacity,size_t(record));
 for(size_t i=0;i<count;++i) out[i]=uint8_t(ssl->offset++);
 ssl->records.front()-=count;
 if(!ssl->records.front()) ssl->records.erase(ssl->records.begin());
 return count;
}
'''
suffix = r'''
int main() {
 uint8_t bytes[1023]; size_t received=0;
 connection.ssl.records={1024,1024,1024,1024};
 assert(receive_tls(nullptr,0,bytes,sizeof(bytes),&received)); assert(received==1023);
 assert(receive_tls(nullptr,0,bytes,sizeof(bytes),&received));
 assert(received==(BL616CL_TLS_COALESCE_READ ? 1023U : 1U));
 for(size_t i=0;i<received;++i) assert(bytes[i]==uint8_t(1023+i));
 connection={};connection.has_peeked=true;connection.peeked=99;
 connection.ssl.records={2,MBEDTLS_ERR_SSL_WANT_READ};
 assert(receive_tls(nullptr,0,bytes,7,&received));
 assert(received==3 && bytes[0]==99 && bytes[1]==0 && bytes[2]==1);
 assert(connection.connected);
 assert(connection.ssl.calls==(BL616CL_TLS_COALESCE_READ ? 2U : 1U));
 for(int terminal:{MBEDTLS_ERR_SSL_WANT_READ,MBEDTLS_ERR_SSL_WANT_WRITE,0,-123}) {
  connection={};connection.ssl.records={1,terminal};
  assert(receive_tls(nullptr,0,bytes,7,&received));assert(received==1);
  assert(connection.connected==(!BL616CL_TLS_COALESCE_READ || terminal==-1 || terminal==-2));
 }
 connection={};connection.ssl.records={-123};
 assert(!receive_tls(nullptr,0,bytes,7,&received));assert(!connection.connected && received==0);
 connection={};connection.ssl.records={1024};
 assert(receive_tls(nullptr,0,bytes,1,&received));assert(received==1 && connection.ssl.calls==1);
 connection={};connection.has_peeked=true;connection.peeked=77;
 assert(receive_tls(nullptr,0,bytes,1,&received));assert(received==1 && bytes[0]==77 && connection.ssl.calls==0);
 assert(!receive_tls(nullptr,1,bytes,1,&received));
 assert(!receive_tls(nullptr,0,bytes,0,&received));
}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(prefix+function+suffix)
    for enabled in (0,1):
        subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                        f'-DBL616CL_TLS_COALESCE_READ={enabled}',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
        subprocess.run([str(path/'test')],check=True)
print('BL616CL_TLS_COALESCE_READ_PASS')
