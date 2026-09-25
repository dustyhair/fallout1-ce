#ifndef FALLOUT_MULTIPLAYER_LOOT_POLICY_H_
#define FALLOUT_MULTIPLAYER_LOOT_POLICY_H_

#include <cstdint>
#include <vector>

#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

struct LootCapShare {
    PlayerId playerId;
    std::uint32_t quantity = 0;
};

inline bool operator==(const LootCapShare& lhs, const LootCapShare& rhs)
{
    return lhs.playerId == rhs.playerId && lhs.quantity == rhs.quantity;
}

// The roster order is stable across peers and save/load. Each odd cap pool
// gives its extra cap to the next player, then advances that cursor.
class LootPolicy {
public:
    bool reset(std::vector<PlayerId> roster);
    bool restore(std::vector<PlayerId> roster, PlayerId nextExtraCapPlayer,
        PlayerId nextItemPriorityPlayer);
    std::vector<LootCapShare> splitCaps(std::uint32_t quantity) const;
    void advanceCaps(std::uint32_t quantity);
    PlayerId itemPriority() const;
    void advanceItemPriority();
    PlayerId nextExtraCapPlayer() const;
    PlayerId nextItemPriorityPlayer() const;

private:
    void advance(std::size_t& cursor);
    std::vector<PlayerId> _roster;
    std::size_t _extraCapCursor = 0;
    std::size_t _itemCursor = 0;
};

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_LOOT_POLICY_H_ */
