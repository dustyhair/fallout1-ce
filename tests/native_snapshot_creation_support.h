#ifndef FALLOUT_TESTS_NATIVE_SNAPSHOT_CREATION_SUPPORT_H_
#define FALLOUT_TESTS_NATIVE_SNAPSHOT_CREATION_SUPPORT_H_

namespace fallout {

struct NativeSnapshotAllocationFailure {
    int requests = 0;
    int failures = 0;
    int bodiesAtFailure = -1;
    int scriptsAtFailure = -1;
};

int nativeSnapshotBodyCount();
int nativeSnapshotScriptCount();
void nativeSnapshotArmObjectFailure(int successfulRequests);
NativeSnapshotAllocationFailure nativeSnapshotDisarmObjectFailure();

} // namespace fallout

#endif
