#include "game/object.cc"
#include "native_snapshot_creation_support.h"

namespace fallout {

int nativeSnapshotBodyCount()
{
    int count = 0;
    for (auto* node = floatingObjects; node != nullptr; node = node->next) ++count;
    for (int tile = 0; tile < HEX_GRID_SIZE; ++tile) {
        for (auto* node = objectTable[tile]; node != nullptr; node = node->next) ++count;
    }
    return count;
}

} // namespace fallout
