#include "multiplayer/local_session.h"

namespace fallout {
namespace multiplayer {

LocalSessionError LocalSession::start(Object* hostActor, Object* guestActor)
{
    if (_active) {
        return LocalSessionError::AlreadyActive;
    }

    if (hostActor == nullptr || guestActor == nullptr) {
        return LocalSessionError::NullActor;
    }

    if (hostActor == guestActor) {
        return LocalSessionError::SameActor;
    }

    _players.clear();

    EntityRegistrationResult host = _entities.registerObject(hostActor, kHostPlayerId);
    if (!host) {
        _players.clear();
        _entities.clear();
        return LocalSessionError::RegistryFailure;
    }

    EntityRegistrationResult guest = _entities.registerObject(guestActor, kGuestPlayerId);
    if (!guest) {
        _players.clear();
        _entities.clear();
        return LocalSessionError::RegistryFailure;
    }

    _hostActorId = host.entityId;
    _guestActorId = guest.entityId;

    PlayerCharacterState hostState;
    hostState.id = kHostPlayerId;
    hostState.actorId = _hostActorId;
    hostState.ownership = PlayerOwnership::LocalControl;
    hostState.connection = ConnectionState::Local;
    PlayerCharacterState guestState;
    guestState.id = kGuestPlayerId;
    guestState.actorId = _guestActorId;
    guestState.ownership = PlayerOwnership::LocalControl;
    guestState.connection = ConnectionState::Local;
    if (_players.registerPlayer(hostState, _entities) != PlayerStateError::None
        || _players.registerPlayer(guestState, _entities) != PlayerStateError::None) {
        _players.clear();
        _entities.clear();
        _hostActorId = {};
        _guestActorId = {};
        return LocalSessionError::RegistryFailure;
    }

    _transports = createLoopbackTransportPair();
    _phase = SessionPhase::Lobby;
    _phaseRevision = 1;
    _active = true;
    return LocalSessionError::None;
}

void LocalSession::stop()
{
    if (_transports.first != nullptr) {
        _transports.first->close();
    }
    if (_transports.second != nullptr) {
        _transports.second->close();
    }

    _transports = {};
    _players.clear();
    _entities.clear();
    _hostActorId = {};
    _guestActorId = {};
    _phase = SessionPhase::Lobby;
    _phaseRevision = 0;
    _active = false;
}

bool LocalSession::isActive() const
{
    return _active;
}

SessionPhase LocalSession::phase() const
{
    return _phase;
}

std::uint32_t LocalSession::phaseRevision() const
{
    return _phaseRevision;
}

LocalSessionError LocalSession::transitionTo(SessionPhase phase)
{
    if (!_active) {
        return LocalSessionError::NotActive;
    }

    if (phase == _phase) {
        return LocalSessionError::None;
    }

    if (!isTransitionAllowed(_phase, phase)) {
        return LocalSessionError::InvalidTransition;
    }

    _phase = phase;
    _phaseRevision++;
    return LocalSessionError::None;
}

EntityId LocalSession::playerActorId(PlayerId playerId) const
{
    if (playerId == kHostPlayerId) {
        return _hostActorId;
    }
    if (playerId == kGuestPlayerId) {
        return _guestActorId;
    }
    return {};
}

LocalSessionError LocalSession::rebindPlayerActor(PlayerId playerId, Object* replacement)
{
    if (!_active) {
        return LocalSessionError::NotActive;
    }

    EntityId entityId = playerActorId(playerId);
    if (!isValid(entityId)) {
        return LocalSessionError::InvalidPlayer;
    }

    if (_entities.rebindObject(entityId, replacement) != EntityRegistryError::None) {
        return LocalSessionError::RegistryFailure;
    }

    return LocalSessionError::None;
}

EntityRegistrationResult LocalSession::registerWorldObject(Object* object)
{
    if (!_active) {
        return { EntityRegistryError::EntityNotFound, {} };
    }

    std::optional<EntityId> existing = _entities.findEntity(object);
    if (existing.has_value()) {
        return { EntityRegistryError::None, *existing };
    }

    return _entities.registerObject(object);
}

void LocalSession::clearWorldEntities()
{
    _entities.removeUnowned();
}

bool LocalSession::owns(PlayerId playerId, EntityId entityId) const
{
    return _active && _entities.isOwnedBy(entityId, playerId);
}

Transport* LocalSession::transportFor(PlayerId playerId)
{
    if (!_active) {
        return nullptr;
    }
    if (playerId == kHostPlayerId) {
        return _transports.first.get();
    }
    if (playerId == kGuestPlayerId) {
        return _transports.second.get();
    }
    return nullptr;
}

const Transport* LocalSession::transportFor(PlayerId playerId) const
{
    if (!_active) {
        return nullptr;
    }
    if (playerId == kHostPlayerId) {
        return _transports.first.get();
    }
    if (playerId == kGuestPlayerId) {
        return _transports.second.get();
    }
    return nullptr;
}

EntityRegistry& LocalSession::entities()
{
    return _entities;
}

const EntityRegistry& LocalSession::entities() const
{
    return _entities;
}

PlayerCharacterStateStore& LocalSession::players()
{
    return _players;
}

const PlayerCharacterStateStore& LocalSession::players() const
{
    return _players;
}

bool LocalSession::isTransitionAllowed(SessionPhase from, SessionPhase to)
{
    switch (from) {
    case SessionPhase::Lobby:
        return to == SessionPhase::Loading || to == SessionPhase::Ending;
    case SessionPhase::Loading:
        return to == SessionPhase::Exploration || to == SessionPhase::Ending;
    case SessionPhase::Exploration:
        return to == SessionPhase::Combat
            || to == SessionPhase::Dialogue
            || to == SessionPhase::Transition
            || to == SessionPhase::Ending;
    case SessionPhase::Combat:
        return to == SessionPhase::Exploration || to == SessionPhase::Ending;
    case SessionPhase::Dialogue:
        return to == SessionPhase::Exploration
            || to == SessionPhase::Combat
            || to == SessionPhase::Ending;
    case SessionPhase::Transition:
        return to == SessionPhase::Exploration || to == SessionPhase::Ending;
    case SessionPhase::Ending:
        return false;
    }

    return false;
}

} // namespace multiplayer
} // namespace fallout
