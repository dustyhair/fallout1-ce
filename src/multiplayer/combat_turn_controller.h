#ifndef FALLOUT_MULTIPLAYER_COMBAT_TURN_CONTROLLER_H_
#define FALLOUT_MULTIPLAYER_COMBAT_TURN_CONTROLLER_H_

#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

// Initiative order is supplied by the authoritative engine. A missing owner
// means that the host, not a peer, drives this actor's AI turn.
struct CombatTurnEntry {
    EntityId actorId;
    std::optional<PlayerId> owner;
};

bool isValidCombatTurnState(const CombatTurnState& state, SessionPhase phase);

enum class CombatTurnResult {
    Accepted,
    InvalidOrder,
    Inactive,
    StaleTurn,
    OutOfTurn,
    Disconnected,
    NotExpired,
    StillConnected,
};

class CombatTurnController {
public:
    CombatTurnResult begin(std::vector<CombatTurnEntry> order,
        std::uint64_t now, std::uint64_t turnDuration,
        std::uint64_t initialRevision = 1, std::uint64_t round = 1);
    bool restore(const CombatTurnState& state, std::uint64_t now);
    CombatTurnState snapshot(std::uint64_t now) const;
    void stop();

    bool active() const;
    const CombatTurnEntry* current() const;
    std::uint64_t revision() const;
    std::uint64_t deadline() const;
    std::uint64_t round() const;

    CombatTurnResult endPlayerTurn(PlayerId playerId, EntityId actorId,
        std::uint64_t revision, std::uint64_t now);
    CombatTurnResult passPlayerTurn(PlayerId playerId, EntityId actorId,
        std::uint64_t revision, std::uint64_t now);
    CombatTurnResult endAiTurn(EntityId actorId, std::uint64_t revision,
        std::uint64_t now);
    CombatTurnResult expirePlayerTurn(PlayerId playerId, EntityId actorId,
        std::uint64_t revision, std::uint64_t now);
    CombatTurnResult passDisconnectedPlayer(PlayerId playerId, EntityId actorId,
        std::uint64_t revision, std::uint64_t now);
    void setConnected(PlayerId playerId, bool connected);

private:
    CombatTurnResult checkTurn(EntityId actorId, std::uint64_t revision) const;
    void advance(std::uint64_t now);

    std::vector<CombatTurnEntry> _order;
    std::unordered_set<PlayerId, PlayerIdHash> _disconnected;
    std::size_t _index = 0;
    std::uint64_t _revision = 0;
    std::uint64_t _deadline = 0;
    std::uint64_t _duration = 0;
    std::uint64_t _round = 0;
};

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_COMBAT_TURN_CONTROLLER_H_ */
