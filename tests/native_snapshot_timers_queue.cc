#include "plib/gnw/memory.h"
#include "native_snapshot_timers_support.h"

namespace fallout {
bool nativeSnapshotTimersQueueAllocation = false;

void* nativeSnapshotTimersQueueMalloc(size_t size)
{
    bool previous = nativeSnapshotTimersQueueAllocation;
    nativeSnapshotTimersQueueAllocation = true;
    void* result = mem_malloc(size);
    nativeSnapshotTimersQueueAllocation = previous;
    return result;
}
} // namespace fallout

// The actual native allocator runs; the wrapper only identifies queue requests
// for the p_malloc failure callback in the private memory translation unit.
#define mem_malloc nativeSnapshotTimersQueueMalloc
#include "game/queue.cc"
#undef mem_malloc
