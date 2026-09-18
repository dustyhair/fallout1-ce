#ifndef FALLOUT_MULTIPLAYER_NETWORK_RUNTIME_H_
#define FALLOUT_MULTIPLAYER_NETWORK_RUNTIME_H_

#include <cstdint>

#include "multiplayer/character_lobby.h"
#include "multiplayer/network_bootstrap.h"

namespace fallout {

struct Object;

namespace multiplayer {

bool networkRuntimeConfigure(int argc, char** argv);
bool networkRuntimeStart();
bool networkRuntimeHost(std::uint16_t port = kDefaultMultiplayerPort);
bool networkRuntimeJoin(const char* endpoint);
void networkRuntimeDisconnect();
NetworkLaunchMode networkRuntimeMode();
bool networkRuntimeConnected();
bool networkRuntimeFailed();
const char* networkRuntimeStatus();
const CharacterCreationSheet* networkRuntimeLocalSheet();
const CharacterCreationSheet* networkRuntimePeerSheet();
bool networkRuntimeSmokeTestEnabled();
bool networkRuntimeRunSmokeTest();
bool networkRuntimeSubmitLocalCharacter(Object* actor);
bool networkRuntimeWaitForLobby();
bool networkRuntimeLobbyReady();
bool networkRuntimeRequestStart();
bool networkRuntimeStartRequested();
bool networkRuntimeEnterWorld();
bool networkRuntimeSubmitLocalMove(int destinationTile, int elevation, bool running);
bool networkRuntimeHandleLocalDoorUse(Object* target);
void networkRuntimeLeaveWorld();
void networkRuntimeStop();

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_NETWORK_RUNTIME_H_ */
