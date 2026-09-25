#ifndef FALLOUT_MULTIPLAYER_NETWORK_RUNTIME_H_
#define FALLOUT_MULTIPLAYER_NETWORK_RUNTIME_H_

#include <cstdint>
#include <optional>
#include <string>

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
bool networkRuntimeIsGuestReplica();
bool networkRuntimeConnected();
bool networkRuntimeFailed();
const char* networkRuntimeStatus();
const CharacterCreationSheet* networkRuntimeLocalSheet();
const CharacterCreationSheet* networkRuntimePeerSheet();
bool networkRuntimeSendChatMessage(const char* text);
std::optional<LobbyChatMessage> networkRuntimeTakeChatMessage();
bool networkRuntimeHandleGameChatInput(int keyCode);
bool networkRuntimeHandleCombatInput(int keyCode);
void networkRuntimeRequestCombatTurnCheckpoint();
bool networkRuntimeSmokeTestEnabled();
const char* networkRuntimeSmokeTestMap();
bool networkRuntimeRunSmokeTest();
bool networkRuntimeSubmitLocalCharacter(Object* actor);
bool networkRuntimeWaitForLobby();
bool networkRuntimeLobbyReady();
bool networkRuntimeRequestStart();
bool networkRuntimeStartRequested();
bool networkRuntimeEnterWorld();
bool networkRuntimeSubmitLocalMove(int destinationTile, int elevation, bool running);
bool networkRuntimeSubmitLocalFacing(int rotation);
bool networkRuntimeSubmitCombatAttack(EntityId targetId, std::int32_t hitMode, std::int32_t hitLocation);
bool networkRuntimeSubmitCombatItem(EntityId itemId, EntityId targetId = {});
bool networkRuntimeSubmitCombatReload(EntityId weaponId, std::int32_t hitMode);
bool networkRuntimeHandleCombatItemUse(Object* item);
bool networkRuntimeHandleCombatReload(Object* weapon, std::int32_t hitMode);
bool networkRuntimeHandleLocalDoorUse(Object* target);
bool networkRuntimeHandleLocalSceneryTransition(Object* target);
bool networkRuntimeHandleLocalPickup(Object* target);
bool networkRuntimeHandleLocalLoot(Object* target);
bool networkRuntimeHandleLocalSkillUse(Object* target, int skill);
bool networkRuntimeHandleLocalItemUse(Object* actor, Object* item, Object* target);
bool networkRuntimeSubmitLocalElevator(std::int32_t elevatorType, std::int32_t destinationLevel);
bool networkRuntimeSubmitLocalExitGrid(EntityId exitId);
bool networkRuntimeSubmitLocalSceneryTransition(EntityId transitionId);
bool networkRuntimeSubmitLocalRest(std::int32_t minutes);
bool networkRuntimeSharedRestEnabled();
std::int32_t networkRuntimePendingRestMinutes();
std::string networkRuntimePendingRestProposerName();
bool networkRuntimeLocalRestProposal();
bool networkRuntimeHandleLocalAttack(Object* target, int hitMode, int hitLocation);
bool networkRuntimeGiveItemToPlayer(EntityId destinationActorId, EntityId itemId, std::uint32_t quantity);
bool networkRuntimeSubmitDirectTrade(const DirectTradeCommand& command);
bool networkRuntimeRequestSharedModal(SharedModalKind kind, bool open);
bool networkRuntimeSubmitLocalWorldMapRoute(std::int32_t targetX, std::int32_t targetY, bool clear = false);
void networkRuntimeFlushWorldMapTerminalEvent();
bool networkRuntimeBlockUnsupportedSharedModal(SharedModalKind kind);
bool networkRuntimeHandleLocalTalk(Object* target);
bool networkRuntimeSubmitLocalTalk(EntityId targetId);
bool networkRuntimeBeginDialogue();
void networkRuntimeEndDialogue();
bool networkRuntimeSubmitDialogueVote(std::uint64_t revision, std::uint8_t option);
void networkRuntimeObserveDialogueDecision(std::uint64_t revision, std::uint8_t option);
void networkRuntimeProcessPendingTalk();
bool networkRuntimeWorldPaused();
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
