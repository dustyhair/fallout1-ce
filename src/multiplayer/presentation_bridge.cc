#include "multiplayer/presentation_bridge.h"

#include "game/object.h"
#include "multiplayer/local_player_context.h"

namespace fallout {
namespace multiplayer {

Object* localPlayerActorOrStoryActor()
{
    Object* actor = localPlayerActor();
    return actor != nullptr ? actor : obj_dude;
}

} // namespace multiplayer
} // namespace fallout
