#ifndef FALLOUT_MULTIPLAYER_LOOT_DISTRIBUTION_CONTROLLER_H_
#define FALLOUT_MULTIPLAYER_LOOT_DISTRIBUTION_CONTROLLER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

struct LootDistributionState {
    std::vector<PlayerId> roster;
    std::uint32_t nextCapExtraIndex = 0;
    std::uint32_t nextLootPriorityIndex = 0;
};

enum class LootDistributionError {
    None,
    InvalidRoster,
    InvalidState,
    InvalidEligiblePlayers,
};

class LootDistributionController {
public:
    LootDistributionError begin(const std::vector<PlayerId>& roster);
    LootDistributionError restore(const LootDistributionState& state);
    std::optional<std::vector<PlayerCapShare>> splitCaps(std::uint32_t caps);
    std::optional<PlayerId> takeLootPriority(const std::vector<PlayerId>& eligible);
    void clear();

    bool active() const { return !_state.roster.empty(); }
    const LootDistributionState& state() const { return _state; }

private:
    LootDistributionState _state;
};

LootDistributionError validateLootDistributionState(
    const LootDistributionState& state);

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_LOOT_DISTRIBUTION_CONTROLLER_H_ */
