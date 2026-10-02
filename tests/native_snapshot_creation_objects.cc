#include <unordered_set>
#include <vector>

#include "game/object.cc"
#include "native_snapshot_creation_support.h"

namespace fallout {

int nativeSnapshotBodyCount()
{
    std::vector<Object*> pending;
    std::unordered_set<Object*> bodies;
    for (auto* node = floatingObjects; node != nullptr; node = node->next) pending.push_back(node->obj);
    for (int tile = 0; tile < HEX_GRID_SIZE; ++tile) {
        for (auto* node = objectTable[tile]; node != nullptr; node = node->next) pending.push_back(node->obj);
    }
    // Adopted inventory children have no map-list node. Count each reachable
    // native body once, including nested containers and duplicate list nodes.
    while (!pending.empty()) {
        Object* body = pending.back();
        pending.pop_back();
        if (body == nullptr || !bodies.insert(body).second) continue;
        const Inventory& inventory = body->data.inventory;
        for (int index = 0; index < inventory.length; ++index) pending.push_back(inventory.items[index].item);
    }
    return static_cast<int>(bodies.size());
}

} // namespace fallout
