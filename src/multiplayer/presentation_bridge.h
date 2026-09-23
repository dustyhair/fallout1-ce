#ifndef FALLOUT_MULTIPLAYER_PRESENTATION_BRIDGE_H_
#define FALLOUT_MULTIPLAYER_PRESENTATION_BRIDGE_H_

namespace fallout {

struct Object;

namespace multiplayer {

Object* localPlayerActorOrStoryActor();
bool isPresentedPlayerActor(const Object* actor);
int updatePlayerGenderAppearance(Object* actor);

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_PRESENTATION_BRIDGE_H_ */
