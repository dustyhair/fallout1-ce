#include "multiplayer/player_character_state.h"

#include <utility>

namespace fallout {
namespace multiplayer {

PlayerStateError PlayerCharacterStateStore::registerPlayer(PlayerCharacterState state, const EntityRegistry& entities)
{
    if (!isValid(state.id)) {
        return PlayerStateError::InvalidPlayerId;
    }
    if (!isValid(state.actorId)) {
        return PlayerStateError::InvalidEntityId;
    }
    if (_players.find(state.id) != _players.end()) {
        return PlayerStateError::PlayerAlreadyRegistered;
    }
    if (_actors.find(state.actorId) != _actors.end()) {
        return PlayerStateError::ActorAlreadyRegistered;
    }
    if (entities.findObject(state.actorId) == nullptr) {
        return PlayerStateError::ActorMissing;
    }
    if (!entities.isOwnedBy(state.actorId, state.id)) {
        return PlayerStateError::ActorNotOwned;
    }

    _actors.emplace(state.actorId, state.id);
    _players.emplace(state.id, std::move(state));
    return PlayerStateError::None;
}

PlayerStateError PlayerCharacterStateStore::setBuild(PlayerId playerId, const CharacterBuild& build)
{
    PlayerCharacterState* state = find(playerId);
    if (state == nullptr) {
        return PlayerStateError::PlayerNotFound;
    }

    state->build = build;
    return PlayerStateError::None;
}

PlayerStateError PlayerCharacterStateStore::setName(PlayerId playerId, const std::string& name)
{
    PlayerCharacterState* state = find(playerId);
    if (state == nullptr) {
        return PlayerStateError::PlayerNotFound;
    }

    state->name = name;
    return PlayerStateError::None;
}

PlayerStateError PlayerCharacterStateStore::setConnection(PlayerId playerId, ConnectionState connection)
{
    PlayerCharacterState* state = find(playerId);
    if (state == nullptr) {
        return PlayerStateError::PlayerNotFound;
    }

    state->connection = connection;
    return PlayerStateError::None;
}

PlayerStateError PlayerCharacterStateStore::unregisterPlayer(PlayerId playerId)
{
    auto player = _players.find(playerId);
    if (player == _players.end()) {
        return PlayerStateError::PlayerNotFound;
    }

    _actors.erase(player->second.actorId);
    _players.erase(player);
    return PlayerStateError::None;
}

PlayerCharacterState* PlayerCharacterStateStore::find(PlayerId playerId)
{
    auto player = _players.find(playerId);
    return player != _players.end() ? &player->second : nullptr;
}

const PlayerCharacterState* PlayerCharacterStateStore::find(PlayerId playerId) const
{
    auto player = _players.find(playerId);
    return player != _players.end() ? &player->second : nullptr;
}

PlayerCharacterState* PlayerCharacterStateStore::findByActor(EntityId actorId)
{
    auto actor = _actors.find(actorId);
    return actor != _actors.end() ? find(actor->second) : nullptr;
}

const PlayerCharacterState* PlayerCharacterStateStore::findByActor(EntityId actorId) const
{
    auto actor = _actors.find(actorId);
    return actor != _actors.end() ? find(actor->second) : nullptr;
}

bool PlayerCharacterStateStore::bindingsMatch(const EntityRegistry& entities) const
{
    for (const auto& entry : _players) {
        const PlayerCharacterState& state = entry.second;
        if (entities.findObject(state.actorId) == nullptr
            || !entities.isOwnedBy(state.actorId, state.id)) {
            return false;
        }
    }
    return true;
}

std::size_t PlayerCharacterStateStore::size() const
{
    return _players.size();
}

void PlayerCharacterStateStore::clear()
{
    _players.clear();
    _actors.clear();
}

} // namespace multiplayer
} // namespace fallout
