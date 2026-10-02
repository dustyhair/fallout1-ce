#include "game/scripts.cc"
#include "native_snapshot_timers_support.h"

namespace fallout {

int nativeSnapshotTimersScriptCount()
{
    int count = 0;
    for (const auto& list : scriptlists) {
        for (auto* extent = list.head; extent != nullptr; extent = extent->next) {
            count += extent->length;
        }
    }
    return count;
}

} // namespace fallout
