#ifndef FALLOUT_MULTIPLAYER_NETWORK_RUNTIME_H_
#define FALLOUT_MULTIPLAYER_NETWORK_RUNTIME_H_

namespace fallout {

struct Object;

namespace multiplayer {

bool networkRuntimeConfigure(int argc, char** argv);
bool networkRuntimeStart();
bool networkRuntimeSubmitLocalCharacter(Object* actor);
bool networkRuntimeWaitForLobby();
bool networkRuntimeLobbyReady();
void networkRuntimeStop();

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_NETWORK_RUNTIME_H_ */
