#include "multiplayer/local_player_context.h"

namespace fallout {
namespace multiplayer {
namespace {

LocalSession* boundSession = nullptr;
PlayerId boundPlayerId;

} // namespace

LocalPlayerError bindLocalPlayer(LocalSession& session, PlayerId playerId)
{
    if (!session.isActive()) {
        return LocalPlayerError::SessionInactive;
    }

    PlayerCharacterState* player = session.players().find(playerId);
    if (player == nullptr) {
        return LocalPlayerError::PlayerMissing;
    }

    if (session.entities().findObject(player->actorId) == nullptr) {
        return LocalPlayerError::ActorMissing;
    }

    boundSession = &session;
    boundPlayerId = playerId;
    return LocalPlayerError::None;
}

void clearLocalPlayer()
{
    boundSession = nullptr;
    boundPlayerId = {};
}

void clearLocalPlayerIfBoundTo(const LocalSession& session)
{
    if (boundSession == &session) {
        clearLocalPlayer();
    }
}

PlayerId localPlayerId()
{
    return localPlayerState() != nullptr ? boundPlayerId : PlayerId {};
}

PlayerCharacterState* localPlayerState()
{
    if (boundSession == nullptr || !boundSession->isActive()) {
        return nullptr;
    }

    return boundSession->players().find(boundPlayerId);
}

Object* localPlayerActor()
{
    PlayerCharacterState* player = localPlayerState();
    return player != nullptr ? boundSession->entities().findObject(player->actorId) : nullptr;
}

PlayerCharacterState* playerStateForActor(const Object* actor)
{
    if (boundSession == nullptr || actor == nullptr) {
        return nullptr;
    }

    std::optional<EntityId> entityId = boundSession->entities().findEntity(const_cast<Object*>(actor));
    return entityId.has_value() ? boundSession->players().findByActor(*entityId) : nullptr;
}

bool isLocalPlayerActor(const Object* actor)
{
    return actor != nullptr && actor == localPlayerActor();
}

ScopedLocalPlayerBinding::ScopedLocalPlayerBinding(LocalSession& session, PlayerId playerId)
    : _previousSession(boundSession)
    , _previousPlayerId(boundPlayerId)
    , _error(bindLocalPlayer(session, playerId))
{
}

ScopedLocalPlayerBinding::~ScopedLocalPlayerBinding()
{
    if (_error != LocalPlayerError::None) {
        return;
    }

    if (_previousSession != nullptr
        && _previousSession->isActive()
        && bindLocalPlayer(*_previousSession, _previousPlayerId) == LocalPlayerError::None) {
        return;
    }
    clearLocalPlayer();
}

ScopedLocalPlayerBinding::operator bool() const
{
    return _error == LocalPlayerError::None;
}

LocalPlayerError ScopedLocalPlayerBinding::error() const
{
    return _error;
}

ScopedLocalPlayerContext::ScopedLocalPlayerContext()
{
    PlayerCharacterState* player = localPlayerState();
    Object* actor = localPlayerActor();
    if (player != nullptr && actor != nullptr) {
        _actingPlayer.emplace(*player, actor);
    }
}

} // namespace multiplayer
} // namespace fallout
