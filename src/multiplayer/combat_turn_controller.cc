#include "multiplayer/combat_turn_controller.h"

#include <limits>
#include <unordered_set>
#include <utility>

namespace fallout {
namespace multiplayer {
namespace {

std::uint64_t deadlineAfter(std::uint64_t now, std::uint64_t duration)
{
    return duration > std::numeric_limits<std::uint64_t>::max() - now
        ? std::numeric_limits<std::uint64_t>::max()
        : now + duration;
}

} // namespace

CombatTurnResult CombatTurnController::begin(std::vector<CombatTurnEntry> order,
    std::uint64_t now, std::uint64_t turnDuration)
{
    std::unordered_set<EntityId, EntityIdHash> seen;
    if (order.empty() || turnDuration == 0) {
        return CombatTurnResult::InvalidOrder;
    }
    for (const CombatTurnEntry& entry : order) {
        if (!isValid(entry.actorId)
            || (entry.owner.has_value() && !isValid(*entry.owner))
            || !seen.insert(entry.actorId).second) {
            return CombatTurnResult::InvalidOrder;
        }
    }

    _order = std::move(order);
    _disconnected.clear();
    _index = 0;
    _revision = 1;
    _round = 1;
    _duration = turnDuration;
    _deadline = deadlineAfter(now, _duration);
    return CombatTurnResult::Accepted;
}

void CombatTurnController::stop()
{
    _order.clear();
    _disconnected.clear();
    _index = 0;
    _revision = 0;
    _deadline = 0;
    _duration = 0;
    _round = 0;
}

bool CombatTurnController::active() const
{
    return !_order.empty();
}

const CombatTurnEntry* CombatTurnController::current() const
{
    return active() ? &_order[_index] : nullptr;
}

std::uint64_t CombatTurnController::revision() const
{
    return _revision;
}

std::uint64_t CombatTurnController::deadline() const
{
    return _deadline;
}

std::uint64_t CombatTurnController::round() const
{
    return _round;
}

CombatTurnResult CombatTurnController::checkTurn(EntityId actorId,
    std::uint64_t revision) const
{
    if (!active()) {
        return CombatTurnResult::Inactive;
    }
    if (revision != _revision) {
        return CombatTurnResult::StaleTurn;
    }
    return current()->actorId == actorId
        ? CombatTurnResult::Accepted
        : CombatTurnResult::OutOfTurn;
}

void CombatTurnController::advance(std::uint64_t now)
{
    _index++;
    if (_index == _order.size()) {
        _index = 0;
        _round++;
    }
    _revision++;
    _deadline = deadlineAfter(now, _duration);
}

CombatTurnResult CombatTurnController::endPlayerTurn(PlayerId playerId,
    EntityId actorId, std::uint64_t revision, std::uint64_t now)
{
    CombatTurnResult result = checkTurn(actorId, revision);
    if (result != CombatTurnResult::Accepted) {
        return result;
    }
    if (!current()->owner.has_value() || *current()->owner != playerId) {
        return CombatTurnResult::OutOfTurn;
    }
    if (_disconnected.find(playerId) != _disconnected.end()) {
        return CombatTurnResult::Disconnected;
    }
    advance(now);
    return CombatTurnResult::Accepted;
}

CombatTurnResult CombatTurnController::endAiTurn(EntityId actorId,
    std::uint64_t revision, std::uint64_t now)
{
    CombatTurnResult result = checkTurn(actorId, revision);
    if (result != CombatTurnResult::Accepted) {
        return result;
    }
    if (current()->owner.has_value()) {
        return CombatTurnResult::OutOfTurn;
    }
    advance(now);
    return CombatTurnResult::Accepted;
}

CombatTurnResult CombatTurnController::expirePlayerTurn(PlayerId playerId,
    EntityId actorId, std::uint64_t revision, std::uint64_t now)
{
    CombatTurnResult result = checkTurn(actorId, revision);
    if (result != CombatTurnResult::Accepted) {
        return result;
    }
    if (!current()->owner.has_value() || *current()->owner != playerId) {
        return CombatTurnResult::OutOfTurn;
    }
    if (now < _deadline) {
        return CombatTurnResult::NotExpired;
    }
    advance(now);
    return CombatTurnResult::Accepted;
}

CombatTurnResult CombatTurnController::passDisconnectedPlayer(PlayerId playerId,
    EntityId actorId, std::uint64_t revision, std::uint64_t now)
{
    CombatTurnResult result = checkTurn(actorId, revision);
    if (result != CombatTurnResult::Accepted) {
        return result;
    }
    if (!current()->owner.has_value() || *current()->owner != playerId) {
        return CombatTurnResult::OutOfTurn;
    }
    if (_disconnected.find(playerId) == _disconnected.end()) {
        return CombatTurnResult::StillConnected;
    }
    advance(now);
    return CombatTurnResult::Accepted;
}

void CombatTurnController::setConnected(PlayerId playerId, bool connected)
{
    if (!isValid(playerId)) {
        return;
    }
    if (connected) {
        _disconnected.erase(playerId);
    } else {
        _disconnected.insert(playerId);
    }
}

} // namespace multiplayer
} // namespace fallout
