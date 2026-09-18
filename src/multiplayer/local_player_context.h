#ifndef FALLOUT_MULTIPLAYER_LOCAL_PLAYER_CONTEXT_H_
#define FALLOUT_MULTIPLAYER_LOCAL_PLAYER_CONTEXT_H_

#include <optional>

#include "multiplayer/acting_player_context.h"
#include "multiplayer/local_session.h"

namespace fallout {
namespace multiplayer {

enum class LocalPlayerError {
    None,
    SessionInactive,
    PlayerMissing,
    ActorMissing,
};

LocalPlayerError bindLocalPlayer(LocalSession& session, PlayerId playerId);
void clearLocalPlayer();
void clearLocalPlayerIfBoundTo(const LocalSession& session);

PlayerId localPlayerId();
PlayerCharacterState* localPlayerState();
Object* localPlayerActor();
PlayerCharacterState* playerStateForActor(const Object* actor);
bool isLocalPlayerActor(const Object* actor);

class ScopedLocalPlayerBinding {
public:
    ScopedLocalPlayerBinding(LocalSession& session, PlayerId playerId);
    explicit ScopedLocalPlayerBinding(Object* actor);
    ~ScopedLocalPlayerBinding();

    ScopedLocalPlayerBinding(const ScopedLocalPlayerBinding&) = delete;
    ScopedLocalPlayerBinding& operator=(const ScopedLocalPlayerBinding&) = delete;

    explicit operator bool() const;
    LocalPlayerError error() const;

private:
    LocalSession* _previousSession = nullptr;
    PlayerId _previousPlayerId;
    LocalPlayerError _error = LocalPlayerError::None;
};

class ScopedLocalPlayerContext {
public:
    ScopedLocalPlayerContext();

    ScopedLocalPlayerContext(const ScopedLocalPlayerContext&) = delete;
    ScopedLocalPlayerContext& operator=(const ScopedLocalPlayerContext&) = delete;

private:
    std::optional<ScopedActingPlayerContext> _actingPlayer;
};

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_LOCAL_PLAYER_CONTEXT_H_ */
