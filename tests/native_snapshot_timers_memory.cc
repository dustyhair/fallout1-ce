#include "plib/gnw/memory.cc"
#include "native_snapshot_timers_support.h"

#include <algorithm>
#include <vector>

namespace fallout {
namespace {
MallocFunc* originalMalloc = nullptr;
FreeFunc* originalFree = nullptr;
int successes = -1;
NativeSnapshotTimersAllocationFailure failure;
std::vector<void*> liveBlocks;

void* failQueueMalloc(size_t size)
{
    if (nativeSnapshotTimersQueueAllocation) {
        ++failure.requests;
        if (successes-- == 0) {
            ++failure.failures;
            failure.bodiesAtFailure = nativeSnapshotTimersBodyCount();
            failure.scriptsAtFailure = nativeSnapshotTimersScriptCount();
            return nullptr;
        }
    }
    void* result = originalMalloc(size);
    if (nativeSnapshotTimersQueueAllocation && result != nullptr) {
        liveBlocks.push_back(result);
    }
    return result;
}

void trackQueueFree(void* pointer)
{
    auto entry = std::find(liveBlocks.begin(), liveBlocks.end(), pointer);
    if (entry != liveBlocks.end()) liveBlocks.erase(entry);
    originalFree(pointer);
}
} // namespace

void nativeSnapshotTimersArmQueueFailure(int successfulRequests)
{
    originalMalloc = p_malloc;
    originalFree = p_free;
    successes = successfulRequests;
    failure = {};
    liveBlocks.clear();
    // Use the native GNW callbacks; count only allocations made inside queue.cc.
    p_malloc = failQueueMalloc;
    p_free = trackQueueFree;
}

NativeSnapshotTimersAllocationFailure nativeSnapshotTimersDisarmQueueFailure()
{
    p_malloc = originalMalloc;
    p_free = originalFree;
    failure.liveBlocks = liveBlocks.size();
    originalMalloc = nullptr;
    originalFree = nullptr;
    successes = -1;
    return failure;
}

} // namespace fallout
