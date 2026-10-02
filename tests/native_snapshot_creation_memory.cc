#include "plib/gnw/memory.cc"
#include "game/object_types.h"
#include "native_snapshot_creation_support.h"

namespace fallout {
namespace {
MallocFunc* snapshotOriginalMalloc = nullptr;
int snapshotSuccessfulRequests = -1;
NativeSnapshotAllocationFailure snapshotFailure;

void* snapshotFailObjectMalloc(size_t size)
{
    if (size == sizeof(Object)) {
        ++snapshotFailure.requests;
        if (snapshotSuccessfulRequests-- == 0) {
            ++snapshotFailure.failures;
            snapshotFailure.bodiesAtFailure = nativeSnapshotBodyCount();
            snapshotFailure.scriptsAtFailure = nativeSnapshotScriptCount();
            return nullptr;
        }
    }
    return snapshotOriginalMalloc(size);
}
} // namespace

void nativeSnapshotArmObjectFailure(int successfulRequests)
{
    snapshotOriginalMalloc = p_malloc;
    snapshotSuccessfulRequests = successfulRequests;
    snapshotFailure = {};
    // The included native allocator remains intact. Only its existing callback
    // changes while the synchronous checkpoint application runs.
    p_malloc = snapshotFailObjectMalloc;
}

NativeSnapshotAllocationFailure nativeSnapshotDisarmObjectFailure()
{
    p_malloc = snapshotOriginalMalloc;
    snapshotOriginalMalloc = nullptr;
    snapshotSuccessfulRequests = -1;
    return snapshotFailure;
}

} // namespace fallout
