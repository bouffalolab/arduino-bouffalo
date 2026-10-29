#!/usr/bin/env python3
"""Execute the actual core parser's sized-response entry with a bounded allocator."""
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import prepare_wifis3_result_error_overlay as overlay
source = (overlay.default_source_library() / 'src/Modem.cpp').read_text()
assert overlay.sha256_file(overlay.default_source_library() / 'src/Modem.cpp') == overlay.MODEM_SOURCE_SHA256
patched = overlay.patch_sized_response_reserve(overlay.patch_modem_source(source))
try:
    overlay.patch_sized_response_reserve(patched)
except RuntimeError:
    pass
else:
    raise AssertionError('duplicate reserve overlay accepted')
def entry(text):
    start = text.index("if(c == '|') {")
    end = text.index("} else if(c == '\\r') {", start)
    return text[start:end] + '}'
program = r'''
#include <cassert>
#include <cstdlib>
#include <new>
#include <string>
#include <algorithm>
static size_t limit=1848, largest=0;
template<class T> struct Limited {
 using value_type=T;
 Limited()=default;
 template<class U> Limited(const Limited<U>&) {}
 T* allocate(size_t n) {
  largest=std::max(largest,n*sizeof(T));
  if(n*sizeof(T)>limit) throw std::bad_alloc();
  auto p=static_cast<T*>(std::malloc(n*sizeof(T)));
  if(!p) throw std::bad_alloc();
  return p;
 }
 void deallocate(T*p,size_t) {std::free(p);}
};
template<class T,class U> bool operator==(Limited<T>,Limited<U>){return true;}
template<class T,class U> bool operator!=(Limited<T>,Limited<U>){return false;}
using Text=std::basic_string<char,std::char_traits<char>,Limited<char>>;
enum class at_parse_state_t {Sized,Res};
static unsigned sized_read_size;
static at_parse_state_t state;
'''
for name, text in [('original',source),('reserved',patched)]:
    program += f'void {name}(Text &data_res) {{ char c=\'|\';\n' + entry(text) + '\n}\n'
program += r'''
int main() {
 bool failed=false;
 try {Text response="1023"; original(response);
  for(unsigned i=0;i<1023;++i) response+=static_cast<char>(i*19);
 } catch(const std::bad_alloc&) {failed=true;}
 assert(failed && largest>limit); // core geometric growth exceeds a 1848B hole
 for(unsigned count: {0U,1U,15U,16U,255U,1023U,1024U}) {
  for(unsigned repeat=0;repeat<64;++repeat) {
   Text response=std::to_string(count).c_str();largest=0;
   reserved(response);
   assert(response.empty() && sized_read_size==count);
   assert(state==(count?at_parse_state_t::Sized:at_parse_state_t::Res));
   for(unsigned i=0;i<count;++i) response+=static_cast<char>(i*19+repeat);
   assert(response.size()==count && largest<=std::max(size_t(31),size_t(count+1)));
   for(unsigned i=0;i<count;++i) assert(response[i]==static_cast<char>(i*19+repeat));
  }
 }
 // Outside the optional 1024B bound, retain the original growth behavior.
 failed=false;
 try {Text response="1025"; reserved(response);
  for(unsigned i=0;i<1025;++i) response+='x';
 } catch(const std::bad_alloc&) {failed=true;}
 assert(failed);
}
'''
with tempfile.TemporaryDirectory(prefix='wifis3-sized-reserve-') as directory:
    work=Path(directory);(work/'test.cpp').write_text(program)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(work/'test.cpp'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
print('WIFIS3_SIZED_RESPONSE_RESERVE_PASS original_growth_rejected=1848 exact_response_bytes=0..1024')
