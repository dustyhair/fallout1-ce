#include "multiplayer/combat_turn_controller.h"

#include <algorithm>
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

bool isValidCombatTurnState(const CombatTurnState& state, SessionPhase phase)
{
    if (phase != SessionPhase::Combat || state.initiative.empty()) {
        return state.revision == 0 && state.round == 0
            && state.activeIndex == 0 && state.remainingMilliseconds == 0
            && state.initiative.empty();
    }
    if (state.revision == 0 || state.round == 0
        || state.initiative.size() > kMaximumCombatInitiative
        || state.activeIndex >= state.initiative.size()) {
        return false;
    }
    std::unordered_set<EntityId, EntityIdHash> seen;
    for (const CombatInitiativeEntry& entry : state.initiative) {
        if (!isValid(entry.actorId) || !seen.insert(entry.actorId).second) {
            return false;
        }
    }
    return true;
}

CombatTurnResult CombatTurnController::begin(std::vector<CombatTurnEntry> order,
    std::uint64_t now, std::uint64_t turnDuration,
    std::uint64_t initialRevision, std::uint64_t round)
{
    std::unordered_set<EntityId, EntityIdHash> seen;
    if (order.empty() || turnDuration == 0 || initialRevision == 0 || round == 0) {
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
    _revision = initialRevision;
    _round = round;
    _duration = turnDuration;
    _deadline = deadlineAfter(now, _duration);
    return CombatTurnResult::Accepted;
}

bool CombatTurnController::restore(const CombatTurnState& state, std::uint64_t now)
{
    if (!isValidCombatTurnState(state, SessionPhase::Combat)) {
        return false;
    }
    std::vector<CombatTurnEntry> order;
    order.reserve(state.initiative.size());
    for (const CombatInitiativeEntry& entry : state.initiative) {
        order.push_back({ entry.actorId,
            isValid(entry.ownerId) ? std::optional<PlayerId>(entry.ownerId) : std::nullopt });
    }
    _order = std::move(order);
    _disconnected.clear();
    _index = state.activeIndex;
    _revision = state.revision;
    _round = state.round;
    _duration = state.remainingMilliseconds;
    _deadline = deadlineAfter(now, _duration);
    return true;
}

CombatTurnState CombatTurnController::snapshot(std::uint64_t now) const
{
    CombatTurnState state;
    if (!active()) {
        return state;
    }
    state.revision = _revision;
    state.round = _round;
    state.activeIndex = static_cast<std::uint32_t>(_index);
    state.remainingMilliseconds = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(_deadline > now ? _deadline - now : 0,
            std::numeric_limits<std::uint32_t>::max()));
    for (const CombatTurnEntry& entry : _order) {
        state.initiative.push_back({ entry.actorId, entry.owner.value_or(PlayerId {}) });
    }
    return state;
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
    return passPlayerTurn(playerId, actorId, revision, now);
}

CombatTurnResult CombatTurnController::passPlayerTurn(PlayerId playerId,
    EntityId actorId, std::uint64_t revision, std::uint64_t now)
{
    CombatTurnResult result = checkTurn(actorId, revision);
    if (result != CombatTurnResult::Accepted) {
        return result;
    }
    if (!current()->owner.has_value() || *current()->owner != playerId) {
        return CombatTurnResult::OutOfTurn;
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
