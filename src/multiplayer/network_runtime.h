#ifndef FALLOUT_MULTIPLAYER_NETWORK_RUNTIME_H_
#define FALLOUT_MULTIPLAYER_NETWORK_RUNTIME_H_

#include <cstdint>
#include <optional>

#include "multiplayer/character_lobby.h"
#include "multiplayer/network_bootstrap.h"
#include "multiplayer/network_lobby.h"

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
bool networkRuntimeSendChatMessage(const char* text);
std::optional<LobbyChatMessage> networkRuntimeTakeChatMessage();
bool networkRuntimeHandleGameChatInput(int keyCode);
bool networkRuntimeSmokeTestEnabled();
bool networkRuntimeRunSmokeTest();
bool networkRuntimeSubmitLocalCharacter(Object* actor);
bool networkRuntimeWaitForLobby();
bool networkRuntimeLobbyReady();
bool networkRuntimeRequestStart();
bool networkRuntimeStartRequested();
bool networkRuntimeEnterWorld();
bool networkRuntimeSubmitLocalMove(int destinationTile, int elevation, bool running);
bool networkRuntimeSubmitLocalFacing(int rotation);
bool networkRuntimeHandleLocalDoorUse(Object* target);
bool networkRuntimeHandleLocalPickup(Object* target);
bool networkRuntimeHandleLocalLoot(Object* target);
bool networkRuntimeHandleLocalAttack(Object* target, int hitMode, int hitLocation);
bool networkRuntimeHandleLocalLootTargetChange(Object* target);
bool networkRuntimeHandleLocalInventoryTransfer(Object* source,
    Object* destination,
    Object* item,
    std::uint32_t quantity,
    std::uint32_t sourceQuantity);

enum class NetworkInventoryTransferDisposition {
    ApplyLocally,
    DeferToHost,
    Reject,
};

NetworkInventoryTransferDisposition networkRuntimePrepareLocalInventoryTransfer(Object* source,
    Object* destination,
    Object* item,
    std::uint32_t quantity,
    std::uint32_t availableQuantity);

enum class NetworkItemDropDisposition {
    ApplyLocally,
    DeferToHost,
    Reject,
};

NetworkItemDropDisposition networkRuntimePrepareLocalItemDrop(Object* source,
    Object* item,
    std::uint32_t quantity,
    std::uint32_t sourceQuantity);
void networkRuntimeHandleLocalItemDrop(Object* source,
    Object* item,
    std::uint32_t quantity,
    std::uint32_t sourceQuantity);
bool networkRuntimeHandleLocalMoneyDrop(Object* source, Object* item, std::uint32_t quantity);
void networkRuntimeLeaveWorld();
void networkRuntimeStop();

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_NETWORK_RUNTIME_H_ */
