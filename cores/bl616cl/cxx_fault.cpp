// Optional no-exceptions bridge ABI: terminal virtual-call faults need no
// libstdc++ exception machinery or its boot-time emergency allocation pool.
#if defined(BL616CL_MINIMAL_CXX_FAULT) && BL616CL_MINIMAL_CXX_FAULT
#ifdef __EXCEPTIONS
#error "Minimal C++ virtual-call faults require exceptions disabled"
#endif
#include <stdlib.h>

extern "C" [[noreturn]] void __cxa_pure_virtual()
{
    abort();
}

extern "C" [[noreturn]] void __cxa_deleted_virtual()
{
    abort();
}
#endif
