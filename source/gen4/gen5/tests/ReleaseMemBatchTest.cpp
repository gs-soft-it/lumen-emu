// SPDX-License-Identifier: GPL-2.0-only
#include "../engine/src/graphics/guest_gpu/command_processor/releaseMemBatch.h"
#include <cstdio>
#include <cstdlib>
using Libs::Graphics::ReleaseMemBatch;
static void Check(bool ok) { if (!ok) std::abort(); }
int main() {
    ReleaseMemBatch batch;
    Check(!batch.TakePending(1));
    Check(!batch.ConsumePacket(1));
    batch.Request(1);
    for (unsigned packet = 1; packet < ReleaseMemBatch::PacketBudget; ++packet) {
        Check(!batch.ConsumePacket(1));
        batch.Request(1); // Repeated releases cannot restart the budget.
    }
    Check(batch.ConsumePacket(1));
    Check(batch.TakePending(1));
    Check(!batch.TakePending(1));
    batch.Request(2);
    Check(batch.TakePending(2)); // A return/wait boundary need not fill the budget.
    batch.Request(3);
    Check(!batch.TakePending(4)); // Native scheduler already submitted the release.
    batch.Request(5);
    Check(!batch.ConsumePacket(6));
    batch.Request(6);
    for (unsigned packet = 1; packet < ReleaseMemBatch::PacketBudget; ++packet)
        Check(!batch.ConsumePacket(6));
    Check(batch.ConsumePacket(6));
    batch.Reset();
    Check(!batch.TakePending(6));
    std::puts("PASS: ReleaseMem batch packet bound, no restart, boundaries, tick rebasing");
}
