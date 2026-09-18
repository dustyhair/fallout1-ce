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
bool isLocalPlayerActor(const Object* actor);

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
