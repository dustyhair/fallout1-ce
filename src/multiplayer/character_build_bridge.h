#ifndef FALLOUT_MULTIPLAYER_CHARACTER_BUILD_BRIDGE_H_
#define FALLOUT_MULTIPLAYER_CHARACTER_BUILD_BRIDGE_H_

#include "multiplayer/player_character_state.h"

namespace fallout {

struct Object;

namespace multiplayer {

bool captureLegacyCharacterBuild(Object* actor, CharacterBuild& build);

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_CHARACTER_BUILD_BRIDGE_H_ */
