#ifndef FALLOUT_MULTIPLAYER_NETWORK_WORLD_H_
#define FALLOUT_MULTIPLAYER_NETWORK_WORLD_H_

#include <optional>
#include <string>
#include <utility>

#include "game/engine_execution_probe.h"
#include "game/worldmap.h"
#include "multiplayer/character_lobby.h"
#include "multiplayer/command_processor.h"
#include "multiplayer/dialogue_vote_controller.h"
#include "multiplayer/network_bootstrap.h"
#include "multiplayer/snapshot.h"

namespace fallout {

struct Object;
struct MapTransition;

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
bool networkWorldRestoreMultiplayerSave(const MultiplayerSaveSidecar& sidecar,
    Object* savedGuestActor);
bool networkWorldBeginEnding();
bool networkWorldApplyPeerMove(const ActorMovementStartedEvent& movement);
bool networkWorldApplyPeerFacing(const ActorFacingChangedEvent& facing);
bool networkWorldApplyPeerDoorUse(const DoorUseStartedEvent& doorUse);
bool networkWorldApplyPeerPickup(const ItemPickupStartedEvent& pickup);
bool networkWorldApplyPeerPickupCompletion(const ItemPickupCompletedEvent& pickup);
bool networkWorldApplyPeerLoot(const LootStartedEvent& loot);
bool networkWorldApplyPeerSkillUse(const SkillUseStartedEvent& skillUse);
bool networkWorldApplyPeerItemUse(const ItemUseStartedEvent& itemUse);
bool networkWorldApplyPeerElevator(const ElevatorTransitionedEvent& elevator);
bool networkWorldApplyPeerExitGrid(const ExitGridTransitionedEvent& exitGrid);
bool networkWorldApplyPeerWorldMapArrival(const WorldMapArrivedEvent& arrival);
bool networkWorldApplyPeerSceneryTransition(const SceneryTransitionedEvent& transition);
bool networkWorldApplyPeerRest(const RestStateChangedEvent& rest);
int networkWorldPendingRestMinutes();
std::string networkWorldPendingRestProposerName();
bool networkWorldLocalRestProposal();
PlayerId networkWorldPendingRestProposer();
void networkWorldClearRestProposal();
bool networkWorldApplyPeerSharedModal(const SharedModalStateChangedEvent& modal);
bool networkWorldApplyPeerWorldMapRoute(const WorldMapRouteSelectedEvent& route);
bool networkWorldApplyPeerAttack(const AttackStartedEvent& attack);
bool networkWorldApplyInventoryTransfer(const InventoryTransferredEvent& transfer, bool reverse = false);
bool networkWorldApplyCapsDistribution(const CapsDistributedEvent& distribution);
bool networkWorldApplyItemDrop(const ItemDroppedEvent& drop);
bool networkWorldApplyLocalItemDrop(Object* source, Object* item, std::uint32_t quantity);
bool networkWorldBeginLocalLoot(Object* target);
bool networkWorldSetLocalLootTarget(Object* target);
bool networkWorldIsLocalInventoryTransfer(Object* source, Object* destination);
bool networkWorldIsLocalItemDrop(Object* source, Object* item);
bool networkWorldItemUseInProgress();
bool networkWorldCaptureScriptedMapTransition(const MapTransition& transition);
Object* networkWorldScriptedSceneryTransitionActor(Object* requestedActor);
void networkWorldHandleObjectDestroyed(Object* object);
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
bool networkWorldTakePendingTalk(EntityId& actorId, EntityId& targetId);
bool networkWorldPublishDialogue(const std::string& reply,
    const std::vector<std::string>& options);
std::optional<std::uint8_t> networkWorldResolveDialogue();
void networkWorldConsumeDialogueChoice();
const DialoguePresentationEvent* networkWorldDialoguePresentation();
const std::vector<DialogueBallot>& networkWorldDialogueBallots();
std::string networkWorldDialoguePlayerName(PlayerId playerId);
void networkWorldDialogueSetConnected(PlayerId playerId, bool connected);
void networkWorldRecordQuestActivity(int globalVar, int value);
void networkWorldObserveWorldMapDiscoveries();
bool networkWorldApplyPeerSharedActivity(const SharedActivityPublishedEvent& event);
std::vector<SharedActivityEntry> networkWorldSharedActivity();
std::vector<EntityId> networkWorldDialogueSmokeTargets();
bool networkWorldMovePartyNearDialogueTarget(EntityId targetId);
bool networkWorldEndDialogue();
bool networkWorldApplyPeerDialogueRequested(const DialogueRequestedEvent& event);
bool networkWorldApplyPeerDialogueVote(const DialogueVoteRecordedEvent& event);
bool networkWorldApplyPeerDialoguePresentation(const DialoguePresentationEvent& event);
bool networkWorldApplyPeerDirectTrade(const DirectTradeStateChangedEvent& event);
void networkWorldDirectTradeSetConnected(PlayerId playerId, bool connected);
std::optional<DirectTradeState> networkWorldDirectTradeState();
void networkWorldCancelPendingWorldMapProposal();
void networkWorldHostTakeOverWorldMapTravel();
bool networkWorldSynchronizeEnginePhase();
SessionPhase networkWorldPhase();
std::uint32_t networkWorldPhaseRevision();
std::optional<EntityId> networkWorldReadyLocalExitGrid();
bool networkWorldIsWorldMapExitGrid(EntityId exitId);
bool networkWorldSharedModalActive();
bool networkWorldLocalWorldMapController();
std::optional<PlayerId> networkWorldPendingWorldMapProposer();
bool networkWorldWorldMapTravelApproved();
std::optional<std::pair<std::int32_t, std::int32_t>> networkWorldSelectedWorldMapRoute();
WorldMapTravelStepResult networkWorldAdvanceWorldMapTravel();
bool networkWorldFinishWorldMapTravel(WorldMapArrivalKind kind,
    int specialEncounter = 0,
    int forcedMap = -1);
bool networkWorldWorldMapDeparted();
void networkWorldHealRemotePlayersForTravelDay();
bool networkWorldCaptureSnapshot(EventSequence lastIncludedEvent, WorldSnapshot& snapshot);
bool networkWorldApplySnapshot(const WorldSnapshot& snapshot);
bool networkWorldCaptureAuthoritativeState(EventSequence lastIncludedEvent, WorldSnapshot& snapshot);
bool networkWorldApplyAuthoritativeState(const WorldSnapshot& snapshot);
std::optional<EntityId> networkWorldFindEntity(const Object* object);
std::optional<PlayerId> networkWorldCombatOwner(const Object* actor);
bool networkWorldCombatBeginRound(Object* const* actors, int count);
bool networkWorldCombatTurnMatches(const Object* actor);
bool networkWorldCombatActionResolving();
void networkWorldCombatCompleteTurn(Object* actor, std::uint64_t expectedRevision);
void networkWorldCombatSetPlayerConnected(PlayerId playerId, bool connected);
void networkWorldCombatTick();
void networkWorldCombatStop();
std::optional<PlayerId> networkWorldActiveCombatOwner();
std::uint64_t networkWorldCombatTurnRevision();
bool networkWorldApplyPeerCombatTurn(const CombatTurnStateChangedEvent& event);
bool networkWorldApplyPeerCombatAction(const CombatActionResolvedEvent& event);
bool networkWorldApplyPeerPartyExperience(const PartyExperienceAwardedEvent& event);
Object* networkWorldFindObject(EntityId entityId);
Object* networkWorldPlayerActor(PlayerId playerId);
MultiplayerSaveError networkWorldCaptureMultiplayerSave(
    std::uint64_t generation,
    std::uint64_t saveDatDigest,
    const ReconnectToken& guestReconnectToken,
    MultiplayerSaveSidecar& sidecar);
void networkWorldLeave();
bool networkWorldActive();
bool networkWorldReplicaSessionActive();
bool networkWorldInventoryTransferInProgress();
bool networkWorldItemDropInProgress();
PartyExperienceResult networkWorldAwardPartyExperience(int xp);
bool networkWorldRunEngineAuthoritySmokeTest(EngineExecutionProbeCounts& counts);
EntityId networkWorldCombatSmokeTarget();
int networkWorldCombatSmokeMoveTile();
bool networkWorldPrepareCombatAttackSmoke(bool lethal = false);
bool networkWorldCombatSmokeTargetDead();
bool networkWorldPrepareCombatItemSmoke();
EntityId networkWorldCombatSmokeItem();
bool networkWorldCombatSmokeActorHealed();
bool networkWorldPrepareCombatReloadSmoke();
EntityId networkWorldCombatSmokeWeapon();
bool networkWorldCombatSmokeWeaponLoaded();
int networkWorldCombatSmokeAmmoUnits();
bool networkWorldPrepareCombatSeparatedElevationSmoke();
bool networkWorldPrepareCombatScriptStatusSmoke();
bool networkWorldCombatScriptStatusObserved();
bool networkWorldRunPartyExperienceSmokeTest();
std::optional<EntityId> networkWorldPrepareDoorSmokeTest();
std::optional<EntityId> networkWorldPreparePickupSmokeTest();
std::optional<EntityId> networkWorldPrepareLootSmokeTest();
std::optional<EntityId> networkWorldPrepareSkillSmokeTest();
std::optional<EntityId> networkWorldPrepareScenerySmokeTest();
bool networkWorldMutateScenerySmokeTest(EntityId targetId);
std::optional<EntityId> networkWorldPrepareContainerSmokeTest();
bool networkWorldMutateContainerSmokeTest(EntityId targetId);
struct QuestSmokeFixture {
    EntityId targetId;
    EntityId itemId;
};
std::optional<QuestSmokeFixture> networkWorldPrepareQuestSmokeTest();
bool networkWorldVerifyQuestSmokeTest(const QuestSmokeFixture& fixture);
struct ElevatorSmokeFixture {
    std::int32_t elevatorType = -1;
    std::int32_t destinationLevel = -1;
    std::int32_t sourceElevation = -1;
    std::int32_t hostTile = -1;
    std::int32_t destinationElevation = -1;
};
std::optional<ElevatorSmokeFixture> networkWorldPrepareElevatorSmokeTest();
bool networkWorldVerifyElevatorSmokeTest(const ElevatorSmokeFixture& fixture);
struct MapTransitionSmokeFixture {
    std::int32_t elevatorType = -1;
    std::int32_t destinationLevel = -1;
    std::int32_t destinationMap = -1;
    std::int32_t destinationElevation = -1;
    std::int32_t guestCaps = 0;
    EntityId hostActorId;
    EntityId guestActorId;
};
std::optional<MapTransitionSmokeFixture> networkWorldPrepareMapTransitionSmokeTest();
bool networkWorldVerifyMapTransitionSmokeTest(const MapTransitionSmokeFixture& fixture);
struct ExitGridSmokeFixture {
    EntityId exitId;
    std::int32_t destinationMap = -1;
    std::int32_t destinationElevation = -1;
    std::int32_t guestCaps = 0;
    EntityId hostActorId;
    EntityId guestActorId;
};
std::optional<ExitGridSmokeFixture> networkWorldPrepareExitGridSmokeTest();
bool networkWorldVerifyExitGridSmokeTest(const ExitGridSmokeFixture& fixture);
struct SceneryTransitionSmokeFixture {
    EntityId transitionId;
    std::int32_t map = -1;
    std::int32_t sourceMap = -1;
    std::int32_t destinationElevation = -1;
    std::int32_t guestCaps = 0;
    std::int32_t hostTile = -1;
    std::int32_t hostElevation = -1;
    std::int32_t hostRotation = 0;
    std::int32_t guestStartingTile = -1;
    std::int32_t guestStartingElevation = -1;
    EntityId hostActorId;
    EntityId guestActorId;
};
std::optional<SceneryTransitionSmokeFixture> networkWorldPrepareSceneryTransitionSmokeTest(bool typedStairs = false, int targetTile = -1, bool hostReady = false);
bool networkWorldVerifySceneryTransitionSmokeTest(const SceneryTransitionSmokeFixture& fixture);
bool networkWorldVerifyLootRangeSmokeTest(EntityId targetId);
std::optional<EntityId> networkWorldPreparePlayerTransferSmokeTest();
bool networkWorldVerifyPlayerTransferRangeSmokeTest(EntityId itemId);
bool networkWorldPrepareRecoverySmokeTest();
bool networkWorldVerifyRecoverySmokeTest();
bool networkWorldRunSharedModalSmokeTest();

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_NETWORK_WORLD_H_ */
