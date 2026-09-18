#ifndef FALLOUT_MULTIPLAYER_ACTING_PLAYER_CONTEXT_H_
#define FALLOUT_MULTIPLAYER_ACTING_PLAYER_CONTEXT_H_

#include "multiplayer/player_character_state.h"

namespace fallout {

struct Object;

namespace multiplayer {

class ScopedActingPlayerContext {
public:
    ScopedActingPlayerContext(PlayerCharacterState& player, Object* actor);
    ~ScopedActingPlayerContext();

    ScopedActingPlayerContext(const ScopedActingPlayerContext&) = delete;
    ScopedActingPlayerContext& operator=(const ScopedActingPlayerContext&) = delete;

private:
    PlayerCharacterState* _previousPlayer;
    Object* _previousActor;
};

PlayerCharacterState* actingPlayerState();
CharacterBuild* actingCharacterBuild();
Object* actingPlayerActor();
CharacterBuild* actingCharacterBuildFor(const Object* actor);
bool isActingPlayerActor(const Object* actor);

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_ACTING_PLAYER_CONTEXT_H_ */
