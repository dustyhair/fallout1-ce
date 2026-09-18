#ifndef FALLOUT_MULTIPLAYER_LOCAL_SESSION_H_
#define FALLOUT_MULTIPLAYER_LOCAL_SESSION_H_

#include <cstdint>

#include "multiplayer/character_lobby.h"
#include "multiplayer/entity_registry.h"
#include "multiplayer/loopback_transport.h"
#include "multiplayer/player_character_state.h"
#include "multiplayer/types.h"

namespace fallout {

struct Object;

namespace multiplayer {

enum class LocalSessionError {
    None,
    AlreadyActive,
    NotActive,
    NullActor,
    SameActor,
    InvalidPlayer,
    InvalidTransition,
    LobbyNotReady,
    RegistryFailure,
};

class LocalSession {
public:
    ~LocalSession();

    LocalSessionError start(Object* hostActor, Object* guestActor);
    void stop();

    bool isActive() const;
    SessionPhase phase() const;
    std::uint32_t phaseRevision() const;
    LocalSessionError transitionTo(SessionPhase phase);
    CharacterLobbyError submitCharacterSheet(const CharacterCreationSheet& sheet);
    bool characterLobbyReady() const;

    EntityId playerActorId(PlayerId playerId) const;
    LocalSessionError rebindPlayerActor(PlayerId playerId, Object* replacement);
    EntityRegistrationResult registerWorldObject(Object* object);
    void clearWorldEntities();
    bool owns(PlayerId playerId, EntityId entityId) const;

    Transport* transportFor(PlayerId playerId);
    const Transport* transportFor(PlayerId playerId) const;

    EntityRegistry& entities();
    const EntityRegistry& entities() const;
    PlayerCharacterStateStore& players();
    const PlayerCharacterStateStore& players() const;
    const CharacterLobby& characterLobby() const;

private:
    static bool isTransitionAllowed(SessionPhase from, SessionPhase to);

    bool _active = false;
    SessionPhase _phase = SessionPhase::Lobby;
    std::uint32_t _phaseRevision = 0;
    EntityId _hostActorId;
    EntityId _guestActorId;
    EntityRegistry _entities;
    PlayerCharacterStateStore _players;
    CharacterLobby _characterLobby;
    LoopbackTransportPair _transports;
};

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_LOCAL_SESSION_H_ */
