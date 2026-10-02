#include "game/object.cc"
#include "native_snapshot_timers_support.h"

#include <unordered_set>

namespace fallout {
namespace {
void countNativeBody(Object* object, std::unordered_set<Object*>& bodies)
{
    if (object == nullptr || !bodies.insert(object).second) return;
    const auto& inventory = object->data.inventory;
    for (int index = 0; index < inventory.length; ++index) {
        countNativeBody(inventory.items[index].item, bodies);
    }
}
} // namespace

int nativeSnapshotTimersBodyCount()
{
    std::unordered_set<Object*> bodies;
    for (auto* node = floatingObjects; node != nullptr; node = node->next) {
        countNativeBody(node->obj, bodies);
    }
    for (int tile = 0; tile < HEX_GRID_SIZE; ++tile) {
        for (auto* node = objectTable[tile]; node != nullptr; node = node->next) {
            countNativeBody(node->obj, bodies);
        }
    }
    return bodies.size();
}

} // namespace fallout
