#ifndef FALLOUT_MULTIPLAYER_NETWORK_WORLD_H_
#define FALLOUT_MULTIPLAYER_NETWORK_WORLD_H_

#include "multiplayer/character_lobby.h"
#include "multiplayer/network_bootstrap.h"

namespace fallout {
namespace multiplayer {

bool networkWorldEnter(NetworkLaunchMode mode,
    const CharacterCreationSheet& localSheet,
    const CharacterCreationSheet& peerSheet);
void networkWorldLeave();
bool networkWorldActive();

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_NETWORK_WORLD_H_ */
