#ifndef FALLOUT_MULTIPLAYER_NETWORK_WORLD_H_
#define FALLOUT_MULTIPLAYER_NETWORK_WORLD_H_

#include <optional>

#include "game/engine_execution_probe.h"
#include "multiplayer/character_lobby.h"
#include "multiplayer/command_processor.h"
#include "multiplayer/network_bootstrap.h"
#include "multiplayer/snapshot.h"

namespace fallout {

struct Object;

namespace multiplayer {

enum class PartyExperienceResult {
    NotMultiplayer,
    Applied,
    ReplicaIgnored,
    Failed,
};

bool networkWorldEnter(NetworkLaunchMode mode,
    const CharacterCreationSheet& localSheet,
    const CharacterCreationSheet& peerSheet);
bool networkWorldApplyPeerMove(const ActorMovementStartedEvent& movement);
bool networkWorldApplyPeerFacing(const ActorFacingChangedEvent& facing);
bool networkWorldApplyPeerDoorUse(const DoorUseStartedEvent& doorUse);
bool networkWorldApplyPeerPickup(const ItemPickupStartedEvent& pickup);
bool networkWorldApplyPeerPickupCompletion(const ItemPickupCompletedEvent& pickup);
bool networkWorldApplyPeerLoot(const LootStartedEvent& loot);
bool networkWorldApplyPeerSkillUse(const SkillUseStartedEvent& skillUse);
bool networkWorldApplyPeerSharedModal(const SharedModalStateChangedEvent& modal);
bool networkWorldApplyPeerAttack(const AttackStartedEvent& attack);
bool networkWorldApplyInventoryTransfer(const InventoryTransferredEvent& transfer, bool reverse = false);
bool networkWorldApplyItemDrop(const ItemDroppedEvent& drop);
bool networkWorldApplyLocalItemDrop(Object* source, Object* item, std::uint32_t quantity);
bool networkWorldBeginLocalLoot(Object* target);
bool networkWorldSetLocalLootTarget(Object* target);
bool networkWorldIsLocalInventoryTransfer(Object* source, Object* destination);
bool networkWorldIsLocalItemDrop(Object* source, Object* item);
void networkWorldHandleItemReplacement(Object* removed, Object* replacement);
void networkWorldHandleItemSplit(Object* original, Object* remainder);
bool networkWorldDescribeItem(const Object* item, ItemDescriptor& descriptor);
std::optional<EntityId> networkWorldEnsureItemRegistered(Object* item);
void networkWorldResetLastItemSplit();
EntityId networkWorldTakeLastItemSplit();
bool networkWorldBeginLocalPickup(Object* target);
void networkWorldFinishPickup(Object* target, bool succeeded);
AuthoritativeCommandResult networkWorldProcessCommand(const GameCommand& command);
std::optional<GameEvent> networkWorldTakeDeferredEvent();
bool networkWorldSynchronizeEnginePhase();
SessionPhase networkWorldPhase();
std::uint32_t networkWorldPhaseRevision();
bool networkWorldSharedModalActive();
bool networkWorldCaptureSnapshot(EventSequence lastIncludedEvent, WorldSnapshot& snapshot);
bool networkWorldApplySnapshot(const WorldSnapshot& snapshot);
bool networkWorldCaptureAuthoritativeState(EventSequence lastIncludedEvent, WorldSnapshot& snapshot);
bool networkWorldApplyAuthoritativeState(const WorldSnapshot& snapshot);
std::optional<EntityId> networkWorldFindEntity(const Object* object);
Object* networkWorldFindObject(EntityId entityId);
Object* networkWorldPlayerActor(PlayerId playerId);
void networkWorldLeave();
bool networkWorldActive();
bool networkWorldInventoryTransferInProgress();
bool networkWorldItemDropInProgress();
PartyExperienceResult networkWorldAwardPartyExperience(int xp);
bool networkWorldRunEngineAuthoritySmokeTest(EngineExecutionProbeCounts& counts);
bool networkWorldRunPartyExperienceSmokeTest();
std::optional<EntityId> networkWorldPrepareDoorSmokeTest();
std::optional<EntityId> networkWorldPreparePickupSmokeTest();
std::optional<EntityId> networkWorldPrepareLootSmokeTest();
std::optional<EntityId> networkWorldPrepareSkillSmokeTest();
bool networkWorldVerifyLootRangeSmokeTest(EntityId targetId);
std::optional<EntityId> networkWorldPreparePlayerTransferSmokeTest();
bool networkWorldVerifyPlayerTransferRangeSmokeTest(EntityId itemId);
bool networkWorldRunSharedModalSmokeTest();

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_NETWORK_WORLD_H_ */
