#include "plib/gnw/memory.cc"
#include "game/object_types.h"
#include "native_snapshot_creation_support.h"

namespace fallout {
namespace {
MallocFunc* snapshotOriginalMalloc = nullptr;
ReallocFunc* snapshotOriginalRealloc = nullptr;
int snapshotSuccessfulRequests = -1;
bool inventoryFailure = false;
int baselineBodies = 0;
int expectedNewBodies = 0;
size_t expectedInventoryBytes = 0;
NativeSnapshotAllocationFailure snapshotFailure;

void* snapshotFailObjectMalloc(size_t size)
{
    if ((!inventoryFailure && size == sizeof(Object))
        || (inventoryFailure && size == expectedInventoryBytes
            && nativeSnapshotBodyCount() >= baselineBodies + expectedNewBodies)) {
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
void* snapshotFailInventoryRealloc(void* pointer, size_t size)
{
    if (inventoryFailure && size == expectedInventoryBytes
        && nativeSnapshotBodyCount() >= baselineBodies + expectedNewBodies) {
        ++snapshotFailure.requests;
        if (snapshotSuccessfulRequests-- == 0) {
            ++snapshotFailure.failures;
            snapshotFailure.bodiesAtFailure = nativeSnapshotBodyCount();
            snapshotFailure.scriptsAtFailure = nativeSnapshotScriptCount();
            return nullptr;
        }
    }
    return snapshotOriginalRealloc(pointer, size);
}
} // namespace

void nativeSnapshotArmObjectFailure(int successfulRequests)
{
    snapshotOriginalMalloc = p_malloc;
    snapshotOriginalRealloc = p_realloc;
    inventoryFailure = false;
    baselineBodies = nativeSnapshotBodyCount();
    snapshotSuccessfulRequests = successfulRequests;
    snapshotFailure = {};
    // The included native allocator remains intact. Only its existing callback
    // changes while the synchronous checkpoint application runs.
    p_malloc = snapshotFailObjectMalloc;
    p_realloc = snapshotFailInventoryRealloc;
}

void nativeSnapshotArmInventoryFailure(int stagedBodies, int capacity)
{
    nativeSnapshotArmObjectFailure(0);
    inventoryFailure = true;
    expectedNewBodies = stagedBodies;
    expectedInventoryBytes = sizeof(InventoryItem) * capacity;
}

NativeSnapshotAllocationFailure nativeSnapshotDisarmObjectFailure()
{
    p_malloc = snapshotOriginalMalloc;
    p_realloc = snapshotOriginalRealloc;
    snapshotOriginalRealloc = nullptr;
    snapshotOriginalMalloc = nullptr;
    snapshotSuccessfulRequests = -1;
    return snapshotFailure;
}

} // namespace fallout
