#ifndef FALLOUT_TESTS_NATIVE_SNAPSHOT_TIMERS_SUPPORT_H_
#define FALLOUT_TESTS_NATIVE_SNAPSHOT_TIMERS_SUPPORT_H_

namespace fallout {

struct NativeSnapshotTimersAllocationFailure {
    int requests = 0;
    int failures = 0;
    int bodiesAtFailure = -1;
    int scriptsAtFailure = -1;
    int liveBlocks = 0;
};

int nativeSnapshotTimersBodyCount();
int nativeSnapshotTimersScriptCount();
extern bool nativeSnapshotTimersQueueAllocation;
void nativeSnapshotTimersArmQueueFailure(int successfulRequests);
NativeSnapshotTimersAllocationFailure nativeSnapshotTimersDisarmQueueFailure();

} // namespace fallout

#endif
