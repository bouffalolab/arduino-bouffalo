#include "runtime_allocation_diagnostic.h"
#include <assert.h>

int main() {
    bridge::RuntimeAllocationDiagnostic stats;
    // Failed OCRAM attempt, successful WRAM fallback, successful ANY result.
    stats.event(13, 8192, 0, 2);
    stats.event(15, 0, 8192, 0);
    stats.event(13, 8192, 0, 3);
    stats.event(15, 1234, 8192, 0);
    stats.event(11, 1234, 8192, 0);
    assert(stats.rows[0].calls == 1 && stats.rows[0].failures == 0);
    assert(stats.rows[4].failures == 1 && stats.rows[4].max_failed_request == 8192);
    assert(stats.rows[5].calls == 1 && stats.rows[5].failures == 0);
    // Explicit WRAM failure and aligned OCRAM request must retain heap identity.
    stats.event(11, 0, 4096, 3);
    stats.event(11, 1234, 128, 64 | 2);
    assert(stats.rows[2].failures == 1 && stats.rows[2].max_failed_request == 4096);
    assert(stats.rows[1].calls == 1 && stats.rows[1].max_request == 128);
    stats.event(12, 0, 4096, 3); // Do not count post-unlock duplicate.
    assert(stats.rows[2].calls == 1);
    stats.rows[2].calls = UINT32_MAX;
    stats.rows[2].failures = UINT32_MAX;
    stats.event(11, 0, 8192, 3);
    assert(stats.rows[2].calls == UINT32_MAX && stats.rows[2].failures == UINT32_MAX);
}
