#pragma once
#include <stdint.h>
#ifndef BL616CL_TCP_RX_BUDGET_DIAGNOSTIC
#define BL616CL_TCP_RX_BUDGET_DIAGNOSTIC 0
#endif
namespace bridge {
// PCB fields are read under the lwIP core lock. Heap stats are a separate
// native-allocator sample: clone/free on other tasks may change them meanwhile.
// Window debit is sequence credit, not bytes of allocated memory or netconn data.
struct TcpRxBudgetDiagnostic {
    enum Field {
        PoolCapacity, PoolUsed, Active, Established, OtherActive, Bound, TimeWait,
        CapTotal, AcceptTotal, WindowDebitTotal, AnnouncedOutstandingTotal,
        AnnouncedOverAccept, MinAccept, MaxAccept, MaxAnnouncedOutstanding,
        ZeroWindows, OoseqSegments, OoseqPbufs, OoseqBytes,
        RefusedPbufs, RefusedBytes, HeapCapacity, HeapUsed, HeapPeak, HeapErrors,
        Count
    };
    uint32_t values[Count] = {};
};
}
