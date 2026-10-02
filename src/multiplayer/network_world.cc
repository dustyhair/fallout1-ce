#include "multiplayer/network_world.h"
#include "multiplayer/network_runtime.h"
#include "multiplayer/character_advancement.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <limits>
#include <optional>
#include <thread>
#include <tuple>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "game/actions.h"
#include "game/automap.h"
#include "game/anim.h"
#include "game/art.h"
#include "game/combat.h"
#include "game/critter.h"
#include "game/display.h"
#include "game/engine_execution_probe.h"
#include "game/elevator.h"
#include "game/game.h"
#include "game/game_vars.h"
#include "game/gdialog.h"
#include "game/intface.h"
#include "game/inventry.h"
#include "game/item.h"
#include "game/map.h"
#include "game/map_defs.h"
#include "game/object.h"
#include "game/party.h"
#include "game/pipboy.h"
#include "game/perk.h"
#include "game/protinst.h"
#include "game/proto.h"
#include "game/queue.h"
#include "game/roll.h"
#include "game/reaction.h"
#include "game/scripts.h"
#include "game/skill.h"
#include "game/stat.h"
#include "game/tile.h"
#include "game/worldmap.h"
#include "int/support/intextra.h"
#include "multiplayer/acting_player_context.h"
#include "multiplayer/combat_turn_controller.h"
#include "multiplayer/dialogue_vote_controller.h"
#include "multiplayer/direct_trade_controller.h"
#include "multiplayer/local_player_context.h"
#include "multiplayer/local_session.h"
#include "multiplayer/loot_distribution_controller.h"
#include "multiplayer/presentation_bridge.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/input.h"
#include "plib/gnw/memory.h"
#include "plib/gnw/svga.h"

namespace fallout {
namespace multiplayer {
namespace {

LocalSession session;
SnapshotCaptureDiagnostic lastCaptureDiagnostic;
SnapshotCaptureDiagnostic lastReportedCaptureDiagnostic;
Object* peerActor = nullptr;
CommandProcessor commandProcessor;
CombatTurnController combatTurns;
// Inventory access is charged once, as in handle_inventory. A turn change
// invalidates access even if a disconnected client never sends CloseInventory.
std::unordered_map<EntityId, std::uint64_t, EntityIdHash> openInventories;
std::unordered_map<EntityId, std::uint64_t, EntityIdHash> openLootTurns;
bool combatActionResolving = false;
constexpr std::uint64_t kCombatTurnDurationMilliseconds = 60000;

std::uint64_t combatClockMilliseconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::vector<std::pair<EntityId, Object*>> worldDoors;
std::vector<std::pair<EntityId, Object*>> worldScenery;
std::vector<std::pair<EntityId, Object*>> worldExitGrids;
std::vector<std::pair<EntityId, Object*>> worldItems;
std::vector<std::pair<EntityId, Object*>> worldCritters;
std::unordered_set<EntityId, EntityIdHash> reservedPickupTargets;
struct PendingPickup {
    EntityId actorId;
    CommandSequence commandSequence;
};
std::unordered_map<EntityId, PendingPickup, EntityIdHash> pendingPickups;
std::deque<GameEvent> deferredEvents;
DialogueVoteController dialogueVotes;
DirectTradeController directTradeController;
std::optional<NpcBarterState> npcBarterState;
std::optional<std::pair<EntityId, EntityId>> pendingScriptedNpcBarter;
std::uint64_t nextNpcBarterRevision = 1;
LootDistributionController lootDistribution;
std::optional<DialoguePresentationEvent> dialoguePresentation;
struct PendingTalk {
    EntityId actorId;
    EntityId targetId;
    CommandSequence causedBy;
};
std::optional<PendingTalk> pendingTalk;
struct PendingCombatStart {
    EntityId actorId;
    StartCombatCommand command;
    CommandSequence causedBy;
    bool started = false;
};
std::optional<PendingCombatStart> pendingCombatStart;
CommandSequence dialogueCause;
std::uint64_t nextDialogueRevision = 1;
std::deque<SharedActivityEntry> sharedActivity;
std::uint64_t nextSharedActivityId = 1;
std::uint32_t observedFirstVisits = 0;

Object* playerFeedbackActor = nullptr;
std::unordered_map<Object*, Object*> activeLootTargets;
struct TheftAccess {
    Object* target;
    int attempts = 0;
    int experience = 0;
    int nextExperience = 10;
};
std::unordered_map<Object*, TheftAccess> theftAccess;
void finishTheft(Object* actor, bool caught);
struct ActiveSharedModal {
    EntityId actorId;
    SharedModalKind kind = SharedModalKind::Dialogue;
};
std::optional<ActiveSharedModal> activeSharedModal;
extern NetworkLaunchMode worldMode;
void publishSharedActivity(SharedActivityKind kind, std::int32_t subject,
    std::int32_t value, const std::string& text)
{
    if (worldMode != NetworkLaunchMode::Host || !session.isActive()
        || text.empty()) return;
    PlayerId sourceId;
    if (const PlayerCharacterState* acting = actingPlayerState()) {
        sourceId = acting->id;
    } else if (activeSharedModal.has_value()
        && activeSharedModal->kind == SharedModalKind::Dialogue) {
        if (const PlayerCharacterState* talker = session.players().findByActor(
                activeSharedModal->actorId)) sourceId = talker->id;
    }
    const PlayerCharacterState* source = session.players().find(sourceId);
    SharedActivityEntry entry;
    entry.id = nextSharedActivityId++;
    entry.sourceId = sourceId;
    entry.sourceName = source != nullptr ? source->name : "World";
    if (entry.sourceName.size() > 32) entry.sourceName.resize(32);
    entry.kind = kind;
    entry.subject = subject;
    entry.value = value;
    entry.text = text.substr(0, 160);
    sharedActivity.push_back(entry);
    if (sharedActivity.size() > 64) sharedActivity.pop_front();
    deferredEvents.push_back(GameEvent { {},
        dialogueCause.value != 0 ? dialogueCause : CommandSequence { UINT64_MAX },
        SharedActivityPublishedEvent {
            session.playerActorId(kHostPlayerId), std::move(entry) } });
}
EntityId approvedWorldMapProposerActorId;
CommandSequence approvedWorldMapProposalSequence;
std::optional<std::pair<std::int32_t, std::int32_t>> selectedWorldMapRoute;
bool worldMapDeparted = false;
int worldMapOriginX = -1;
int worldMapOriginY = -1;
struct PendingWorldMapProposal {
    EntityId proposerActorId;
    int map = -1;
    std::uint32_t phaseRevision = 0;
    CommandSequence proposalSequence;
    std::chrono::steady_clock::time_point expiresAt;
    std::unordered_set<PlayerId, PlayerIdHash> readyPlayers;
    EntityId sourceEntityId;
    int sourceTile = -1;
    int sourceElevation = -1;
    bool proposerMustStandOnSource = false;
};
std::optional<PendingWorldMapProposal> pendingWorldMapProposal;
bool inventoryTransferInProgress = false;
bool itemDropInProgress = false;
bool itemUseInProgress = false;
struct PendingDialogueMapTransition {
    MapTransition destination;
    EntityId actorId;
    CommandSequence cause;
};
std::optional<PendingDialogueMapTransition> pendingDialogueMapTransition;
bool scriptedSceneryTransitionInProgress = false;
std::optional<MapTransition> capturedSceneryMapTransition;
struct PendingRestProposal {
    std::int32_t minutes = 0;
    PlayerId proposer;
    int map = -1;
    std::uint32_t phaseRevision = 0;
    std::chrono::steady_clock::time_point expiresAt;
    std::unordered_set<PlayerId, PlayerIdHash> readyPlayers;
};
std::optional<PendingRestProposal> pendingRestProposal;

bool allConnectedPlayersReady(const std::unordered_set<PlayerId, PlayerIdHash>& readyPlayers)
{
    if (session.players().size() == 0) {
        return false;
    }
    for (PlayerId playerId : session.players().playerIds()) {
        if (readyPlayers.find(playerId) == readyPlayers.end()) {
            return false;
        }
    }
    return true;
}

bool allConnectedPlayersNear(int tile, int elevation, int radius)
{
    if (!hexGridTileIsValid(tile) || !elevationIsValid(elevation)
        || session.players().size() == 0) {
        return false;
    }
    for (PlayerId playerId : session.players().playerIds()) {
        Object* actor = session.entities().findObject(session.playerActorId(playerId));
        if (actor == nullptr || actor->elevation != elevation
            || tile_dist(actor->tile, tile) > radius) {
            return false;
        }
    }
    return true;
}

struct PlayerActorSlot {
    PlayerId playerId;
    EntityId actorId;
    Object* actor = nullptr;
};

bool captureFailure(SnapshotCaptureFailure failure, const char* section,
    EntityId entityId = {}, SnapshotError error = SnapshotError::None)
{
    lastCaptureDiagnostic = { failure, error, section, entityId,
        map_data.field_34, session.phase(), session.players().playerIds().size(),
        worldCritters.size(), worldDoors.size(), worldScenery.size(), worldItems.size() };
    if (lastReportedCaptureDiagnostic.failure != failure
        || lastReportedCaptureDiagnostic.snapshotError != error
        || lastReportedCaptureDiagnostic.section != section
        || lastReportedCaptureDiagnostic.entityId != entityId
        || lastReportedCaptureDiagnostic.mapId != map_data.field_34) {
        std::fprintf(stderr,
            "MULTIPLAYER_SNAPSHOT_CAPTURE_FAILED reason=%d section=%s error=%d entity=%u map=%d phase=%d actors=%zu critters=%zu doors=%zu scenery=%zu items=%zu.\n",
            static_cast<int>(failure), section, static_cast<int>(error), entityId.value,
            lastCaptureDiagnostic.mapId, static_cast<int>(lastCaptureDiagnostic.phase),
            lastCaptureDiagnostic.actors, lastCaptureDiagnostic.critters, lastCaptureDiagnostic.doors,
            lastCaptureDiagnostic.scenery, lastCaptureDiagnostic.items);
        lastReportedCaptureDiagnostic = lastCaptureDiagnostic;
    }
    return false;
}

std::optional<std::vector<PlayerActorSlot>> playerActorRoster()
{
    std::vector<PlayerId> ids = session.players().playerIds();
    if (ids.empty() || ids.size() > kMaximumTransitionPlayers) return std::nullopt;
    std::vector<PlayerActorSlot> roster;
    roster.reserve(ids.size());
    std::unordered_set<EntityId, EntityIdHash> seenActors;
    for (PlayerId playerId : ids) {
        EntityId actorId = session.playerActorId(playerId);
        Object* actor = session.entities().findObject(actorId);
        if (!isValid(actorId) || actor == nullptr
            || !seenActors.insert(actorId).second) return std::nullopt;
        roster.push_back(PlayerActorSlot { playerId, actorId, actor });
    }
    return roster;
}

bool capturePlayerPlacementRoster(std::vector<PlayerTransitionPlacement>& placements)
{
    auto roster = playerActorRoster();
    if (!roster.has_value()) return false;
    placements.clear();
    placements.reserve(roster->size());
    for (const PlayerActorSlot& slot : *roster) {
        if (!hexGridTileIsValid(slot.actor->tile)
            || !elevationIsValid(slot.actor->elevation)
            || slot.actor->rotation < 0 || slot.actor->rotation >= ROTATION_COUNT) {
            return false;
        }
        placements.push_back(PlayerTransitionPlacement {
            slot.playerId, slot.actorId, slot.actor->tile,
            slot.actor->elevation, slot.actor->rotation,
        });
    }
    return true;
}

bool applyPlayerPlacementRoster(const std::vector<PlayerTransitionPlacement>& placements)
{
    auto roster = playerActorRoster();
    if (!roster.has_value() || placements.size() != roster->size()) return false;
    std::unordered_set<PlayerId, PlayerIdHash> seen;
    for (const PlayerTransitionPlacement& placement : placements) {
        auto slot = std::find_if(roster->begin(), roster->end(), [&](const PlayerActorSlot& entry) {
            return entry.playerId == placement.playerId;
        });
        if (slot == roster->end() || slot->actorId != placement.actorId
            || !seen.insert(placement.playerId).second
            || !hexGridTileIsValid(placement.tile)
            || !elevationIsValid(placement.elevation)
            || placement.rotation < 0 || placement.rotation >= ROTATION_COUNT) {
            return false;
        }
        for (const PlayerTransitionPlacement& prior : placements) {
            if (&prior == &placement) break;
            if (prior.tile == placement.tile && prior.elevation == placement.elevation) return false;
        }
    }
    for (const PlayerTransitionPlacement& placement : placements) {
        Object* actor = session.entities().findObject(placement.actorId);
        register_clear(actor);
        if (obj_move_to_tile(actor, placement.tile, placement.elevation, nullptr) == -1
            || obj_set_rotation(actor, placement.rotation, nullptr) == -1) {
            return false;
        }
    }
    return true;
}

bool placePlayerRosterAtDestination(PlayerId leadingPlayer,
    int tile,
    int elevation,
    int rotation,
    bool keepLeaderPosition = false)
{
    auto roster = playerActorRoster();
    if (!roster.has_value() || !hexGridTileIsValid(tile)
        || !elevationIsValid(elevation)
        || rotation < 0 || rotation >= ROTATION_COUNT) {
        return false;
    }
    auto leader = std::find_if(roster->begin(), roster->end(), [&](const PlayerActorSlot& entry) {
        return entry.playerId == leadingPlayer;
    });
    if (leader == roster->end()) return false;
    // A world-map map load has already placed the host. Asking Fallout to
    // place it again treats its own hex as blocked and shifts the spawn.
    if ((!keepLeaderPosition
            && obj_attempt_placement(leader->actor, tile, elevation, 0) == -1)
        || (keepLeaderPosition
            && (leader->actor->tile != tile || leader->actor->elevation != elevation))) {
        return false;
    }
    for (const PlayerActorSlot& slot : *roster) {
        if (slot.playerId == leadingPlayer) continue;
        if (obj_attempt_placement(slot.actor, tile, elevation, 2) == -1) return false;
    }
    std::vector<PlayerTransitionPlacement> placements;
    if (!capturePlayerPlacementRoster(placements)) return false;
    for (const PlayerTransitionPlacement& placement : placements) {
        if (placement.elevation != elevation) return false;
    }
    for (std::size_t index = 0; index < placements.size(); index++) {
        for (std::size_t prior = 0; prior < index; prior++) {
            if (placements[index].tile == placements[prior].tile
                && placements[index].elevation == placements[prior].elevation) {
                return false;
            }
        }
    }
    for (const PlayerActorSlot& slot : *roster) {
        if (obj_set_rotation(slot.actor, rotation, nullptr) == -1) return false;
    }
    return true;
}

bool placeReadyElevatorRiders(PlayerId actingPlayer,
    int sourceTile,
    int sourceElevation,
    int destinationTile,
    int destinationElevation,
    std::vector<PlayerId>& riders)
{
    auto roster = playerActorRoster();
    std::vector<PlayerTransitionPlacement> previous;
    if (!roster.has_value() || !capturePlayerPlacementRoster(previous)) return false;
    auto actor = std::find_if(roster->begin(), roster->end(), [&](const PlayerActorSlot& slot) {
        return slot.playerId == actingPlayer;
    });
    if (actor == roster->end()) return false;
    riders.clear();
    riders.push_back(actingPlayer);
    for (const PlayerActorSlot& slot : *roster) {
        if (slot.playerId != actingPlayer
            && slot.actor->elevation == sourceElevation
            && tile_dist(slot.actor->tile, sourceTile) <= 4) {
            riders.push_back(slot.playerId);
        }
    }
    for (PlayerId playerId : riders) {
        auto slot = std::find_if(roster->begin(), roster->end(), [&](const PlayerActorSlot& entry) {
            return entry.playerId == playerId;
        });
        register_clear(slot->actor);
    }
    bool placed = obj_attempt_placement(actor->actor,
        destinationTile, destinationElevation, 0) != -1;
    for (std::size_t index = 1; placed && index < riders.size(); index++) {
        auto slot = std::find_if(roster->begin(), roster->end(), [&](const PlayerActorSlot& entry) {
            return entry.playerId == riders[index];
        });
        placed = obj_attempt_placement(slot->actor,
            destinationTile, destinationElevation, 2) != -1;
    }
    std::vector<PlayerTransitionPlacement> resulting;
    placed = placed && capturePlayerPlacementRoster(resulting);
    for (PlayerId playerId : riders) {
        auto placement = std::find_if(resulting.begin(), resulting.end(), [&](const PlayerTransitionPlacement& entry) {
            return entry.playerId == playerId;
        });
        if (placement == resulting.end() || placement->elevation != destinationElevation) {
            placed = false;
            break;
        }
    }
    for (std::size_t index = 0; placed && index < resulting.size(); index++) {
        for (std::size_t prior = 0; prior < index; prior++) {
            if (resulting[index].tile == resulting[prior].tile
                && resulting[index].elevation == resulting[prior].elevation) {
                placed = false;
                break;
            }
        }
    }
    if (!placed) applyPlayerPlacementRoster(previous);
    return placed;
}

bool fillElevatorExecutionFromRoster(ElevatorExecution& execution)
{
    std::vector<PlayerTransitionPlacement> placements;
    if (!capturePlayerPlacementRoster(placements) || placements.size() != 2) return false;
    for (const PlayerTransitionPlacement& placement : placements) {
        if (placement.playerId == kHostPlayerId) {
            execution.hostTile = placement.tile;
            execution.hostElevation = placement.elevation;
            execution.hostRotation = placement.rotation;
        } else if (placement.playerId == kGuestPlayerId) {
            execution.guestTile = placement.tile;
            execution.guestElevation = placement.elevation;
            execution.guestRotation = placement.rotation;
        } else {
            return false;
        }
    }
    return execution.hostTile >= 0 && execution.guestTile >= 0;
}

bool worldMapProposalSourceReady()
{
    if (!pendingWorldMapProposal.has_value()
        || !isValid(pendingWorldMapProposal->sourceEntityId)) {
        return true;
    }
    const PendingWorldMapProposal& proposal = *pendingWorldMapProposal;
    Object* source = session.entities().findObject(proposal.sourceEntityId);
    Object* proposer = session.entities().findObject(proposal.proposerActorId);
    return source != nullptr
        && proposer != nullptr
        && source->tile == proposal.sourceTile
        && source->elevation == proposal.sourceElevation
        && allConnectedPlayersNear(proposal.sourceTile, proposal.sourceElevation, 4)
        && (!proposal.proposerMustStandOnSource
            || (proposer->tile == proposal.sourceTile
                && proposer->elevation == proposal.sourceElevation));
}
constexpr auto kRestProposalLifetime = std::chrono::seconds(90);
constexpr auto kWorldMapProposalLifetime = std::chrono::seconds(90);
NetworkLaunchMode worldMode = NetworkLaunchMode::Disabled;
StoryPresentationState storyPresentation;

void queueCombatTurnState()
{
    if (!session.isActive() || worldMode != NetworkLaunchMode::Host) {
        return;
    }
    deferredEvents.push_back(GameEvent {
        {},
        CommandSequence { 1 }, // Engine-originated combat boundary.
        CombatTurnStateChangedEvent {
            session.playerActorId(kHostPlayerId),
            session.phase(),
            session.phaseRevision(),
            combatTurns.snapshot(combatClockMilliseconds()),
        },
    });
}
EntityId expectedSplitEntityId;
EntityId lastSplitEntityId;
int questSmokeInitialReputation = 0;

constexpr int kJarvisScriptIndex = 439;
constexpr int kJarvisCuredLocalVariable = 5;
constexpr int kAntidotePid = 49;
constexpr int kChildrenIndependentStairPid = 0x200015C;
constexpr int kChildrenIndependentStairTile = 18900;


bool isExitGrid(const Object* object)
{
    return object != nullptr
        && FID_TYPE(object->fid) == OBJ_TYPE_MISC
        && object->pid >= PROTO_ID_0x5000010
        && object->pid <= PROTO_ID_0x5000017;
}

bool sceneryTransitionType(const Object* object, int& sceneryType)
{
    if (object == nullptr || FID_TYPE(object->fid) != OBJ_TYPE_SCENERY) {
        return false;
    }
    Proto* proto = nullptr;
    if (proto_ptr(object->pid, &proto) == -1) {
        return false;
    }
    sceneryType = proto->scenery.type;
    return sceneryType == SCENERY_TYPE_STAIRS
        || sceneryType == SCENERY_TYPE_LADDER_UP
        || sceneryType == SCENERY_TYPE_LADDER_DOWN;
}

bool typedStairCanBeUsedIndependently(const Object* stair, int sourceMap)
{
    // A scripted stair with no declared destination may request a cross-map
    // load after executing world-changing code. Only independently verified
    // installed routes may run without the other player nearby.
    return sourceMap == MAP_CHILDRN2
        && stair->pid == kChildrenIndependentStairPid
        && stair->tile == kChildrenIndependentStairTile
        && stair->elevation == 0;
}

bool isWorldMapDestination(int map)
{
    // Native map_leave_map normalizes 0 to -2. -1 opens the town map
    // before world travel; -2 opens world travel directly.
    return map >= -2 && map <= 0;
}

bool exitGridDestination(const Object* exitGrid,
    int& map,
    int& tile,
    int& elevation,
    int& rotation)
{
    if (!isExitGrid(exitGrid)) {
        return false;
    }
    map = exitGrid->data.misc.map;
    tile = exitGrid->data.misc.tile;
    elevation = exitGrid->data.misc.elevation;
    rotation = exitGrid->data.misc.rotation;
    if (isWorldMapDestination(map)) {
        // World travel has no destination map placement yet.
        return true;
    }
    return map > 0
        && hexGridTileIsValid(tile)
        && elevationIsValid(elevation)
        && rotation >= 0
        && rotation < ROTATION_COUNT;
}

bool registerWorldObjects();
Object* createPeerActor();

std::uint32_t sharedObjectFlags(const Object* object)
{
    return static_cast<std::uint32_t>(object->flags) & kSharedObjectFlagMask;
}

bool applySharedObjectPresentation(Object* object,
    std::int32_t fid,
    std::int32_t frame,
    std::uint32_t objectFlags,
    std::int32_t lightDistance,
    std::int32_t lightIntensity)
{
    if (object == nullptr || (objectFlags & ~kSharedObjectFlagMask) != 0) {
        return false;
    }
    bool resetOffsets = FID_TYPE(fid) == OBJ_TYPE_CRITTER && FID_ANIM_TYPE(fid) == ANIM_STAND
        && (object->x != 0 || object->y != 0);
    bool changed = object->fid != fid || object->frame != frame
        || sharedObjectFlags(object) != objectFlags
        || object->lightDistance != lightDistance || object->lightIntensity != lightIntensity
        || resetOffsets;
    Rect oldBounds;
    obj_bound(object, &oldBounds);
    Rect dirtyRect {};
    if (FID_TYPE(fid) == OBJ_TYPE_CRITTER && FID_ANIM_TYPE(fid) == ANIM_STAND) {
        // Checkpoints own replica poses. Even an idle animation can keep adding
        // frame offsets if its frame is repeatedly reset by network updates.
        // Stop that local sequence and discard its accumulated displacement.
        register_clear(object);
        if ((object->x != 0 || object->y != 0)
            && obj_move_to_tile(object, object->tile, object->elevation, &dirtyRect) == -1) {
            return false;
        }
    }
    if ((object->fid != fid && obj_change_fid(object, fid, &dirtyRect) == -1)
        || (object->frame != frame && obj_set_frame(object, frame, &dirtyRect) == -1)) {
        return false;
    }
    if (object->lightDistance != lightDistance || object->lightIntensity != lightIntensity) {
        obj_set_light(object, lightDistance, lightIntensity, &dirtyRect);
    }
    object->flags = static_cast<int>((static_cast<std::uint32_t>(object->flags) & ~kSharedObjectFlagMask)
        | objectFlags);
    if (changed) {
        tile_refresh_rect(&dirtyRect, object->elevation);
        tile_refresh_rect(&oldBounds, object->elevation);
        obj_bound(object, &dirtyRect);
        tile_refresh_rect(&dirtyRect, object->elevation);
    }
    return true;
}

bool captureVariables(const int* variables, int count, std::vector<std::int32_t>& captured)
{
    if (count < 0
        || static_cast<std::size_t>(count) > kMaxSnapshotVariables
        || (count != 0 && variables == nullptr)) {
        return false;
    }
    captured.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; index++) {
        captured.push_back(variables[index]);
    }
    return true;
}

bool validateGameGlobalVariables(const WorldSnapshot& snapshot)
{
    return num_game_global_vars >= 0
        && snapshot.gameGlobalVariables.size() == static_cast<std::size_t>(num_game_global_vars)
        && (num_game_global_vars == 0 || game_global_vars != nullptr);
}

bool validateMapGlobalVariables(const WorldSnapshot& snapshot)
{
    return num_map_global_vars >= 0
        && snapshot.mapGlobalVariables.size() == static_cast<std::size_t>(num_map_global_vars)
        && (num_map_global_vars == 0 || map_global_vars != nullptr);
}

bool validateVariableState(const WorldSnapshot& snapshot)
{
    return validateGameGlobalVariables(snapshot)
        && validateMapGlobalVariables(snapshot)
        && num_map_local_vars >= 0
        && snapshot.mapLocalVariables.size() == static_cast<std::size_t>(num_map_local_vars)
        && (num_map_local_vars == 0 || map_local_vars != nullptr);
}

void applyVariableState(const WorldSnapshot& snapshot)
{
    if (!snapshot.gameGlobalVariables.empty()) {
        std::copy(snapshot.gameGlobalVariables.begin(), snapshot.gameGlobalVariables.end(), game_global_vars);
    }
    if (!snapshot.mapGlobalVariables.empty()) {
        std::copy(snapshot.mapGlobalVariables.begin(), snapshot.mapGlobalVariables.end(), map_global_vars);
    }
    if (!snapshot.mapLocalVariables.empty()) {
        std::copy(snapshot.mapLocalVariables.begin(), snapshot.mapLocalVariables.end(), map_local_vars);
    }
}

bool captureTimedEvents(WorldSnapshot& snapshot)
{
    std::vector<QueueEventState> queueEvents;
    if (!queue_capture_state(queueEvents)) {
        return captureFailure(SnapshotCaptureFailure::NativeQueue, "timers");
    }
    if (queueEvents.size() > kMaxSnapshotTimedEvents) {
        return captureFailure(SnapshotCaptureFailure::SnapshotValidation, "timers", {}, SnapshotError::TooManyTimedEvents);
    }
    snapshot.timedEvents.reserve(queueEvents.size());
    for (const QueueEventState& queueEvent : queueEvents) {
        TimedEventSnapshot event;
        event.time = queueEvent.time;
        event.eventType = static_cast<std::uint8_t>(queueEvent.eventType);
        event.payloadCount = static_cast<std::uint8_t>(queueEvent.payloadCount);
        for (std::size_t index = 0; index < queueEvent.payload.size(); index++) {
            event.payload[index] = queueEvent.payload[index];
        }
        // Script timed-event processing resolves the script by SID and ignores
        // the legacy owner pointer. Normalizing it avoids depending on an
        // unreplicated scenery object's process-local identity.
        if (queueEvent.owner != nullptr && queueEvent.eventType != EVENT_TYPE_SCRIPT) {
            std::optional<EntityId> ownerId = session.entities().findEntity(queueEvent.owner);
            if (!ownerId.has_value()) {
                if (lastReportedCaptureDiagnostic.failure != SnapshotCaptureFailure::MissingTimerOwner
                    || lastReportedCaptureDiagnostic.mapId != map_data.field_34) {
                    std::fprintf(stderr, "MULTIPLAYER_SNAPSHOT_TIMER_OWNER_MISSING type=%d time=%d.\n",
                        queueEvent.eventType, queueEvent.time);
                }
                return captureFailure(SnapshotCaptureFailure::MissingTimerOwner, "timers");
            }
            event.ownerId = *ownerId;
        }
        snapshot.timedEvents.push_back(event);
    }
    return true;
}


bool validateActorState(const WorldSnapshot& snapshot)
{
    if (snapshot.actors.size() != session.players().playerIds().size()) {
        return false;
    }
    for (const ActorSnapshot& actorState : snapshot.actors) {
        Object* actor = session.entities().findObject(actorState.entityId);
        std::optional<PlayerId> owner = session.entities().ownerOf(actorState.entityId);
        PlayerCharacterState* player = session.players().find(actorState.ownerId);
        if (actor == nullptr
            || !owner.has_value()
            || *owner != actorState.ownerId
            || player == nullptr
            || player->actorId != actorState.entityId) {
            return false;
        }
    }
    return true;
}

bool validateCritterState(const WorldSnapshot& snapshot)
{
    for (const CritterSnapshot& critterState : snapshot.critters) {
        Object* critter = session.entities().findObject(critterState.entityId);
        if (critter == nullptr
            || critter->pid != critterState.pid
            || FID_TYPE(critter->fid) != OBJ_TYPE_CRITTER) {
            return false;
        }
    }
    return true;
}

bool validateSnapshotPrototypes(const WorldSnapshot& snapshot)
{
    auto available = [](int pid, int type, const char* section) {
        Proto* prototype = nullptr;
        if (proto_ptr(pid, &prototype) != 0 || prototype == nullptr
            || FID_TYPE(prototype->fid) != type) {
            std::fprintf(stderr,
                "Multiplayer snapshot prototype preflight failed: section=%s pid=%d type=%d.\n",
                section, pid, type);
            return false;
        }
        return true;
    };
    for (const CritterSnapshot& critter : snapshot.critters) {
        if (!available(critter.pid, OBJ_TYPE_CRITTER, "critters")) return false;
    }
    for (const ScenerySnapshot& scenery : snapshot.scenery) {
        if (!available(scenery.pid, OBJ_TYPE_SCENERY, "scenery")) return false;
    }
    for (const ItemSnapshot& item : snapshot.items) {
        if (!available(item.itemDescriptor.pid, OBJ_TYPE_ITEM, "items")) return false;
    }
    return true;
}

bool validateCritterArtCatalog(std::int32_t fid, const char* section)
{
    // Native death/electrify art aliases index anon_alias before art_get_name
    // checks the catalog bound. Validate that input before any art cache call.
    char baseName[14];
    if (art_get_base_name(OBJ_TYPE_CRITTER, fid & 0xFFF, baseName) != 0) {
        std::fprintf(stderr,
            "Multiplayer snapshot art catalog preflight failed: section=%s fid=%d.\n",
            section, fid);
        return false;
    }
    return true;
}

bool validateSnapshotActorArt(const WorldSnapshot& snapshot)
{
    for (const ActorSnapshot& state : snapshot.actors) {
        if (!validateCritterArtCatalog(state.fid, "actors")) return false;
        Object* actor = session.entities().findObject(state.entityId);
        // Roster identity has already been checked. Unchanged presentation
        // does not call obj_set_frame and may intentionally have no artwork.
        if (actor != nullptr && actor->fid == state.fid && actor->frame == state.frame) continue;
        CacheEntry* handle = nullptr;
        Art* frames = art_ptr_lock(state.fid, &handle);
        bool available = frames != nullptr && state.frame < art_frame_max_frame(frames);
        if (frames != nullptr) art_ptr_unlock(handle);
        if (!available) {
            std::fprintf(stderr,
                "Multiplayer snapshot actor art preflight failed: entity=%u fid=%d frame=%d.\n",
                state.entityId.value, state.fid, state.frame);
            return false;
        }
    }
    // NPC identities may be rebuilt, so entity ID alone cannot establish an
    // unchanged presentation exemption. Only protect native alias indexing
    // here; NPC frame/resource reconciliation remains a separate boundary.
    for (const CritterSnapshot& state : snapshot.critters) {
        if (!validateCritterArtCatalog(state.fid, "critters")) return false;
    }
    return true;
}

Object* matchSnapshotCritter(const CritterSnapshot& state,
    const std::vector<Object*>& candidates, const std::unordered_set<Object*>& used)
{
    for (Object* candidate : candidates) {
        if (candidate != nullptr && used.count(candidate) == 0
            && candidate->pid == state.pid && candidate->tile == state.tile
            && candidate->elevation == state.elevation) return candidate;
    }
    Object* solePidMatch = nullptr;
    for (Object* candidate : candidates) {
        if (candidate != nullptr && used.count(candidate) == 0 && candidate->pid == state.pid) {
            if (solePidMatch != nullptr) return nullptr;
            solePidMatch = candidate;
        }
    }
    return solePidMatch;
}

struct SnapshotReconciliationPlan {
    std::unordered_map<EntityId, Object*, EntityIdHash> objects;
    std::vector<const ItemSnapshot*> orderedItems;
    std::vector<Object*> created;
    struct InventoryStorage {
        Object* holder;
        InventoryItem* items;
        int capacity;
    };
    std::vector<InventoryStorage> inventories;
    bool reconstructScenery = false;
    bool reconstructCritters = false;

    SnapshotReconciliationPlan() = default;
    SnapshotReconciliationPlan(const SnapshotReconciliationPlan&) = delete;
    SnapshotReconciliationPlan& operator=(const SnapshotReconciliationPlan&) = delete;
    ~SnapshotReconciliationPlan();

    Object* find(EntityId id) const
    {
        auto entry = objects.find(id);
        return entry != objects.end() ? entry->second : nullptr;
    }

    void commit()
    {
        // Reserve native holder storage before any reconciliation mutation.
        for (const InventoryStorage& storage : inventories) {
            Inventory& inventory = storage.holder->data.inventory;
            mem_free(inventory.items);
            inventory.items = storage.items;
            inventory.capacity = storage.capacity;
        }
        inventories.clear();
        // Native lifetime callbacks own every body after reconciliation starts.
        created.clear();
    }
};

bool validateSnapshotCritterFrames(const WorldSnapshot& snapshot, const SnapshotReconciliationPlan& plan)
{
    for (const CritterSnapshot& state : snapshot.critters) {
        Object* matched = plan.find(state.entityId);
        // Reconstructed native objects start at frame zero. Presentation only
        // loads the selected FID's artwork when obj_set_frame will be called.
        int nativeFrame = matched != nullptr ? matched->frame : 0;
        if (nativeFrame == state.frame) continue;
        CacheEntry* handle = nullptr;
        Art* frames = art_ptr_lock(state.fid, &handle);
        bool available = frames != nullptr && state.frame < art_frame_max_frame(frames);
        if (frames != nullptr) art_ptr_unlock(handle);
        if (!available) {
            std::fprintf(stderr,
                "Multiplayer snapshot critter frame preflight failed: entity=%u fid=%d frame=%d native_frame=%d matched=%d.\n",
                state.entityId.value, state.fid, state.frame, nativeFrame, matched != nullptr ? 1 : 0);
            return false;
        }
    }
    return true;
}

bool applyActorAndCritterState(const WorldSnapshot& snapshot, bool preserveMovement)
{
    for (const ActorSnapshot& actorState : snapshot.actors) {
        Object* actor = session.entities().findObject(actorState.entityId);
        if (session.players().setBuild(actorState.ownerId, actorState.build) != PlayerStateError::None) {
            std::fprintf(stderr, "Snapshot actor build failed id=%u owner=%u.\n",
                actorState.entityId.value, actorState.ownerId.value);
            return false;
        }
        Rect oldBounds;
        obj_bound(actor, &oldBounds);
        int oldElevation = actor->elevation;
        Rect dirtyRect {};
        bool dirty = false;
        int localAnimation = FID_ANIM_TYPE(actor->fid);
        int authoritativeAnimation = FID_ANIM_TYPE(actorState.fid);
        bool continuingPath = preserveMovement && snapshot.phase == SessionPhase::Exploration
            && actor->elevation == actorState.elevation
            && (localAnimation == ANIM_WALK || localAnimation == ANIM_RUNNING)
            && (authoritativeAnimation == ANIM_WALK || authoritativeAnimation == ANIM_RUNNING)
            && anim_busy(actor) != 0 && tile_dist(actor->tile, actorState.tile) <= 2;
        bool movementStopped = (localAnimation == ANIM_WALK || localAnimation == ANIM_RUNNING)
            && authoritativeAnimation != ANIM_WALK && authoritativeAnimation != ANIM_RUNNING;
        // A path can still be animating inside the destination hex. Replacing
        // its walking art without stopping it makes subsequent frames keep
        // adding movement offsets even though the authoritative tile is fixed.
        if (!continuingPath && (actor->tile != actorState.tile || actor->elevation != actorState.elevation
                || movementStopped)) {
            register_clear(actor);
            if (obj_move_to_tile(actor, actorState.tile, actorState.elevation, &dirtyRect) == -1) {
                std::fprintf(stderr, "Snapshot actor move failed id=%u from=%d to=%d elev=%d.\n",
                    actorState.entityId.value, actor->tile, actorState.tile,
                    actorState.elevation);
                return false;
            }
            dirty = true;
        }
        if (!continuingPath && actor->rotation != actorState.rotation) {
            if (obj_set_rotation(actor, actorState.rotation, &dirtyRect) == -1) {
                std::fprintf(stderr, "Snapshot actor rotation failed id=%u.\n",
                    actorState.entityId.value);
                return false;
            }
            dirty = true;
        }
        if (!applySharedObjectPresentation(actor, continuingPath ? actor->fid : actorState.fid,
                continuingPath ? actor->frame : actorState.frame, actorState.objectFlags,
                actorState.lightDistance, actorState.lightIntensity)) {
            std::fprintf(stderr, "Snapshot actor presentation failed id=%u.\n",
                actorState.entityId.value);
            return false;
        }
        // A replica applies the host's selected death and animation state; it
        // must never call critter_kill through critter_adjust_hits here.
        actor->data.critter.hp = actorState.hitPoints;
        actor->data.critter.poison = actorState.poison;
        actor->data.critter.radiation = actorState.radiation;
        actor->data.critter.combat.ap = actorState.actionPoints;
        actor->data.critter.combat.results = actorState.combatResults;
        actor->data.critter.combat.maneuver = actorState.combatManeuver;
        actor->data.critter.combat.damageLastTurn = actorState.damageLastTurn;
        actor->data.critter.combat.team = actorState.team;
        if (dirty) {
            tile_refresh_rect(&dirtyRect, actorState.elevation);
        }
        if (dirty) tile_refresh_rect(&oldBounds, oldElevation);
    }
    for (const CritterSnapshot& critterState : snapshot.critters) {
        Object* critter = session.entities().findObject(critterState.entityId);
        Rect oldBounds;
        obj_bound(critter, &oldBounds);
        int oldElevation = critter->elevation;
        Rect dirtyRect {};
        bool dirty = false;
        int localAnimation = FID_ANIM_TYPE(critter->fid);
        int authoritativeAnimation = FID_ANIM_TYPE(critterState.fid);
        bool movementStopped = (localAnimation == ANIM_WALK || localAnimation == ANIM_RUNNING)
            && authoritativeAnimation != ANIM_WALK && authoritativeAnimation != ANIM_RUNNING;
        if (critter->tile != critterState.tile || critter->elevation != critterState.elevation
            || movementStopped) {
            register_clear(critter);
            if (obj_move_to_tile(critter, critterState.tile, critterState.elevation, &dirtyRect) == -1) {
                std::fprintf(stderr, "Snapshot critter move failed id=%u from=%d to=%d elev=%d.\n",
                    critterState.entityId.value, critter->tile, critterState.tile,
                    critterState.elevation);
                return false;
            }
            dirty = true;
        }
        if (critter->rotation != critterState.rotation) {
            if (obj_set_rotation(critter, critterState.rotation, &dirtyRect) == -1) {
                return false;
            }
            dirty = true;
        }
        if (!applySharedObjectPresentation(critter, critterState.fid,
                critterState.frame, critterState.objectFlags,
                critterState.lightDistance, critterState.lightIntensity)) {
            return false;
        }
        critter->data.critter.hp = critterState.hitPoints;
        critter->data.critter.combat.ap = critterState.actionPoints;
        critter->data.critter.combat.results = critterState.combatResults;
        critter->data.critter.combat.team = critterState.team;
        critter->data.critter.combat.maneuver = critterState.combatManeuver;
        critter->data.critter.combat.damageLastTurn = critterState.damageLastTurn;
        // Replicas retain native companion membership for inventory, travel
        // and save/load lifecycle; only authority runs companion scripts/AI.
        if (critterState.partyMember != isPartyMember(critter)) {
            int result = critterState.partyMember ? partyMemberAdd(critter) : partyMemberRemove(critter);
            if (result != 0) return false;
        }
        if (dirty) {
            tile_refresh_rect(&dirtyRect, critterState.elevation);
        }
        if (dirty) tile_refresh_rect(&oldBounds, oldElevation);
    }
    for (const ActorSnapshot& actorState : snapshot.actors) {
        Object* actor = session.entities().findObject(actorState.entityId);
        actor->data.critter.combat.whoHitMe = isValid(actorState.whoHitMeId)
            ? session.entities().findObject(actorState.whoHitMeId) : nullptr;
    }
    for (const CritterSnapshot& critterState : snapshot.critters) {
        Object* critter = session.entities().findObject(critterState.entityId);
        critter->data.critter.combat.whoHitMe = isValid(critterState.whoHitMeId)
            ? session.entities().findObject(critterState.whoHitMeId) : nullptr;
    }
    return true;
}

bool describeItem(const Object* item, ItemDescriptor& descriptor)
{
    if (item == nullptr || FID_TYPE(item->fid) != OBJ_TYPE_ITEM || PID_TYPE(item->pid) != OBJ_TYPE_ITEM) {
        return false;
    }
    descriptor.pid = item->pid;
    descriptor.extendedFlags = item->data.flags;
    descriptor.data0 = item->data.item.weapon.ammoQuantity;
    descriptor.data1 = item->data.item.weapon.ammoTypePid;
    return true;
}

bool applyItemDescriptor(Object* item, const ItemDescriptor& descriptor)
{
    if (item == nullptr
        || !hasItemDescriptor(descriptor)
        || item->pid != descriptor.pid
        || FID_TYPE(item->fid) != OBJ_TYPE_ITEM) {
        return false;
    }
    item->data.flags = descriptor.extendedFlags;
    item->data.item.weapon.ammoQuantity = descriptor.data0;
    item->data.item.weapon.ammoTypePid = descriptor.data1;
    return true;
}

bool itemDescriptorsEqual(const ItemDescriptor& left, const ItemDescriptor& right)
{
    return left.pid == right.pid
        && left.extendedFlags == right.extendedFlags
        && left.data0 == right.data0
        && left.data1 == right.data1;
}

bool snapshotSceneryLayoutMismatch(const WorldSnapshot& snapshot)
{
    if (snapshot.scenery.size() != worldScenery.size()) return true;
    for (std::size_t index = 0; index < snapshot.scenery.size(); ++index) {
        if (worldScenery[index].second == nullptr
            || worldScenery[index].second->pid != snapshot.scenery[index].pid) return true;
    }
    return false;
}

Object* matchSnapshotScenery(const ScenerySnapshot& state,
    const std::vector<std::pair<EntityId, Object*>>& candidates, const std::unordered_set<Object*>& used)
{
    for (const auto& entry : candidates) {
        Object* candidate = entry.second;
        if (candidate != nullptr && used.count(candidate) == 0
            && candidate->pid == state.pid && candidate->tile == state.tile
            && candidate->elevation == state.elevation) return candidate;
    }
    return nullptr;
}

std::vector<const ItemSnapshot*> orderSnapshotItems(const WorldSnapshot& snapshot)
{
    // The codec already rejects dangling holders and cycles. Process parents
    // first so both preflight and reconciliation resolve the same containers.
    std::unordered_map<EntityId, const ItemSnapshot*, EntityIdHash> itemStates;
    for (const ItemSnapshot& state : snapshot.items) itemStates.emplace(state.entityId, &state);
    std::unordered_set<EntityId, EntityIdHash> orderedIds;
    std::vector<const ItemSnapshot*> ordered;
    ordered.reserve(snapshot.items.size());
    for (const ItemSnapshot& state : snapshot.items) {
        std::vector<const ItemSnapshot*> chain;
        const ItemSnapshot* current = &state;
        while (current != nullptr && orderedIds.insert(current->entityId).second) {
            chain.push_back(current);
            auto parent = itemStates.find(current->holderId);
            current = parent != itemStates.end() ? parent->second : nullptr;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) ordered.push_back(*it);
    }
    return ordered;
}

Object* matchSnapshotItem(const ItemSnapshot& state, Object* holder, bool hasHolder,
    const std::vector<Object*>& candidates, const std::unordered_set<Object*>& used)
{
    // A holder planned for creation cannot own any existing native item.
    if (hasHolder && holder == nullptr) return nullptr;
    Object* pidFallback = nullptr;
    for (Object* candidate : candidates) {
        if (candidate == nullptr || used.count(candidate) != 0
            || candidate->pid != state.itemDescriptor.pid) continue;
        bool locationMatches = hasHolder ? candidate->owner == holder
            : candidate->owner == nullptr && candidate->tile == state.tile
                && candidate->elevation == state.elevation;
        if (!locationMatches) continue;
        ItemDescriptor descriptor;
        if (describeItem(candidate, descriptor)
            && itemDescriptorsEqual(descriptor, state.itemDescriptor)) return candidate;
        if (pidFallback == nullptr) pidFallback = candidate;
    }
    return pidFallback;
}

bool validateRequiredObjectFrame(std::int32_t fid, std::int32_t frame,
    EntityId id, const char* section)
{
    CacheEntry* handle = nullptr;
    Art* frames = art_ptr_lock(fid, &handle);
    bool available = frames != nullptr && frame < art_frame_max_frame(frames);
    if (frames != nullptr) art_ptr_unlock(handle);
    if (!available) {
        std::fprintf(stderr,
            "Multiplayer snapshot object frame preflight failed: section=%s entity=%u fid=%d frame=%d.\n",
            section, id.value, fid, frame);
    }
    return available;
}

bool validateSnapshotObjectFrames(const WorldSnapshot& snapshot, const SnapshotReconciliationPlan& plan)
{
    // Validate the exact bodies selected for reconciliation. Null means a new
    // frame-zero object, which will be allocated only after art preflight.
    for (const DoorSnapshot& state : snapshot.doors) {
        Object* door = plan.find(state.entityId);
        if (door == nullptr || !obj_is_a_portal(door) || state.open != (state.frame != 0)
            || !validateRequiredObjectFrame(door->fid, state.frame, state.entityId, "doors")) return false;
    }
    for (const ScenerySnapshot& state : snapshot.scenery) {
        Object* matched = plan.find(state.entityId);
        int nativeFrame = matched != nullptr ? matched->frame : 0;
        if (nativeFrame != state.frame
            && !validateRequiredObjectFrame(state.fid, state.frame, state.entityId, "scenery")) return false;
    }
    for (const ItemSnapshot* state : plan.orderedItems) {
        Object* matched = plan.find(state->entityId);
        int nativeFrame = matched != nullptr ? matched->frame : 0;
        if (nativeFrame != state->frame
            && !validateRequiredObjectFrame(state->fid, state->frame, state->entityId, "items")) return false;
    }
    return true;
}

void trackWorldItem(EntityId entityId, Object* item)
{
    auto existing = std::find_if(worldItems.begin(), worldItems.end(), [&](const auto& entry) {
        return entry.first == entityId;
    });
    if (existing == worldItems.end()) {
        worldItems.emplace_back(entityId, item);
    } else {
        existing->second = item;
    }
}

Object* createItem(const ItemDescriptor& descriptor)
{
    Object* item = nullptr;
    if (!hasItemDescriptor(descriptor)
        || obj_pid_new(&item, descriptor.pid) == -1
        || item == nullptr
        || !applyItemDescriptor(item, descriptor)) {
        if (item != nullptr) {
            obj_erase_object(item, nullptr);
        }
        return nullptr;
    }
    return item;
}

SnapshotReconciliationPlan::~SnapshotReconciliationPlan()
{
    for (const InventoryStorage& storage : inventories) mem_free(storage.items);
    for (Object* object : created) {
        // Staged bodies keep their native list node and never own inventory.
        // Remove scripts directly so cleanup cannot execute a DESTROY proc.
        if (object->sid != -1) {
            scr_remove(object->sid);
            object->sid = -1;
        }
        object->flags &= ~OBJECT_NO_REMOVE;
        obj_erase_object(object, nullptr);
    }
}

void selectSnapshotBodies(const WorldSnapshot& snapshot, SnapshotReconciliationPlan& plan)
{
    plan.reconstructScenery = networkWorldReplicaSessionActive() && snapshotSceneryLayoutMismatch(snapshot);
    plan.reconstructCritters = snapshot.critters.size() != worldCritters.size() || !validateCritterState(snapshot);
    plan.orderedItems = orderSnapshotItems(snapshot);
    plan.objects.reserve(snapshot.actors.size() + snapshot.doors.size()
        + snapshot.scenery.size() + snapshot.critters.size() + snapshot.items.size());
    plan.created.reserve(snapshot.scenery.size() + snapshot.critters.size() + snapshot.items.size());
    for (const ActorSnapshot& state : snapshot.actors) {
        plan.objects.emplace(state.entityId, session.entities().findObject(state.entityId));
    }
    for (std::size_t index = 0; index < snapshot.doors.size(); ++index) {
        plan.objects.emplace(snapshot.doors[index].entityId, worldDoors[index].second);
    }
    std::unordered_set<Object*> usedScenery;
    for (std::size_t index = 0; index < snapshot.scenery.size(); ++index) {
        const ScenerySnapshot& state = snapshot.scenery[index];
        Object* matched = plan.reconstructScenery ? matchSnapshotScenery(state, worldScenery, usedScenery)
            : worldScenery[index].second;
        if (matched != nullptr) usedScenery.insert(matched);
        plan.objects.emplace(state.entityId, matched);
    }
    std::vector<Object*> critterCandidates;
    for (const auto& entry : worldCritters) critterCandidates.push_back(entry.second);
    std::unordered_set<Object*> usedCritters;
    for (const CritterSnapshot& state : snapshot.critters) {
        Object* matched = plan.reconstructCritters ? matchSnapshotCritter(state, critterCandidates, usedCritters)
            : session.entities().findObject(state.entityId);
        if (matched != nullptr) usedCritters.insert(matched);
        plan.objects.emplace(state.entityId, matched);
    }
    std::vector<Object*> itemCandidates;
    for (const auto& entry : worldItems) if (entry.second != nullptr) itemCandidates.push_back(entry.second);
    std::unordered_set<Object*> usedItems;
    for (const ItemSnapshot* state : plan.orderedItems) {
        Object* matched = matchSnapshotItem(*state, plan.find(state->holderId),
            isValid(state->holderId), itemCandidates, usedItems);
        if (matched != nullptr) usedItems.insert(matched);
        plan.objects.emplace(state->entityId, matched);
    }
}

bool stageSnapshotBodies(const WorldSnapshot& snapshot, SnapshotReconciliationPlan& plan)
{
    auto create = [&plan](EntityId id, int pid, int type) {
        if (plan.find(id) != nullptr) return true;
        Object* object = nullptr;
        // Discarded creations consume monotonic native IDs, but never entity IDs.
        if (obj_pid_new(&object, pid) == -1 || object == nullptr) return false;
        plan.created.push_back(object);
        object->flags &= ~OBJECT_NO_REMOVE;
        if (type != OBJ_TYPE_ITEM) object->flags |= OBJECT_NO_SAVE;
        Proto* prototype = nullptr;
        if (FID_TYPE(object->fid) != type || proto_ptr(pid, &prototype) == -1 || prototype == nullptr
            || (prototype->sid != -1 && object->sid == -1)) return false;
        plan.objects.at(id) = object;
        return true;
    };
    for (const ScenerySnapshot& state : snapshot.scenery) {
        if (!create(state.entityId, state.pid, OBJ_TYPE_SCENERY)) return false;
    }
    for (const CritterSnapshot& state : snapshot.critters) {
        if (!create(state.entityId, state.pid, OBJ_TYPE_CRITTER)) return false;
    }
    for (const ItemSnapshot* state : plan.orderedItems) {
        bool missing = plan.find(state->entityId) == nullptr;
        if (!create(state->entityId, state->itemDescriptor.pid, OBJ_TYPE_ITEM)
            || (missing && !applyItemDescriptor(plan.find(state->entityId), state->itemDescriptor))) return false;
    }
    // Stage native inventory capacity without changing current holder arrays.
    // Include all incoming bodies as an upper bound, even if native stacking
    // later reduces the number of entries required.
    std::unordered_map<Object*, int> incoming;
    for (const ItemSnapshot* state : plan.orderedItems) {
        if (!isValid(state->holderId)) continue;
        Object* holder = plan.find(state->holderId);
        Object* item = plan.find(state->entityId);
        if (holder == nullptr || item == nullptr) return false;
        if (item->owner != holder) ++incoming[holder];
    }
    plan.inventories.reserve(incoming.size());
    for (const auto& entry : incoming) {
        Inventory& inventory = entry.first->data.inventory;
        if (inventory.length < 0 || inventory.capacity < inventory.length
            || (inventory.length > 0 && inventory.items == nullptr)) return false;
        int required = inventory.length + entry.second;
        if (inventory.items != nullptr && inventory.capacity >= required) continue;
        int capacity = ((required + 9) / 10) * 10;
        auto* items = static_cast<InventoryItem*>(mem_malloc(sizeof(InventoryItem) * capacity));
        if (items == nullptr) return false;
        if (inventory.length > 0) std::copy_n(inventory.items, inventory.length, items);
        plan.inventories.push_back({ entry.first, items, capacity });
    }
    // Move only newly created bodies. obj_move_to_tile reuses their creation
    // node and reports failure; obj_attempt_placement discards that result.
    std::unordered_set<Object*> staged(plan.created.begin(), plan.created.end());
    for (const ScenerySnapshot& state : snapshot.scenery) {
        Object* object = plan.find(state.entityId);
        if (staged.count(object) != 0
            && obj_move_to_tile(object, state.tile, state.elevation, nullptr) == -1) return false;
    }
    for (const CritterSnapshot& state : snapshot.critters) {
        Object* object = plan.find(state.entityId);
        if (staged.count(object) != 0
            && obj_move_to_tile(object, state.tile, state.elevation, nullptr) == -1) return false;
    }
    for (const ItemSnapshot* state : plan.orderedItems) {
        Object* object = plan.find(state->entityId);
        if (!isValid(state->holderId) && staged.count(object) != 0
            && obj_move_to_tile(object, state->tile, state->elevation, nullptr) == -1) return false;
    }
    return true;
}

bool validateItemTimerOwnerFlags(const WorldSnapshot& snapshot)
{
    for (const TimedEventSnapshot& event : snapshot.timedEvents) {
        auto owner = std::find_if(snapshot.items.begin(), snapshot.items.end(), [&](const auto& item) {
            return item.entityId == event.ownerId;
        });
        // Native queues mark every owner USED. In particular, this keeps an
        // item timer owner from being replaced by a native inventory merge.
        if (owner != snapshot.items.end() && (owner->objectFlags & OBJECT_USED) == 0) return false;
    }
    return true;
}

bool prepareTimedEvents(const WorldSnapshot& snapshot, const SnapshotReconciliationPlan& reconciliation,
    std::vector<Object*>& owners, PreparedQueueEvents& prepared)
{
    std::vector<QueueEventState> events;
    events.reserve(snapshot.timedEvents.size());
    owners.reserve(snapshot.timedEvents.size());
    for (const TimedEventSnapshot& event : snapshot.timedEvents) {
        QueueEventState native;
        native.time = event.time;
        native.eventType = event.eventType;
        native.payloadCount = event.payloadCount;
        native.payload = event.payload;
        if (isValid(event.ownerId)) {
            native.owner = reconciliation.find(event.ownerId);
            if (native.owner == nullptr) return false;
        }
        owners.push_back(native.owner);
        events.push_back(native);
    }
    return prepared.prepare(events);
}

EntityRegistrationResult registerItem(Object* item)
{
    EntityRegistrationResult registration = session.registerWorldObject(item);
    if (registration) {
        trackWorldItem(registration.entityId, item);
    }
    return registration;
}

Object* topEnvironmentOrSelf(Object* object)
{
    Object* top = obj_top_environment(object);
    return top != nullptr ? top : object;
}

bool lootTargetIsValid(Object* actor, Object* target)
{
    return actor != nullptr
        && target != nullptr
        && actor != target
        && ((FID_TYPE(target->fid) == OBJ_TYPE_CRITTER && !critter_is_active(target))
            || (FID_TYPE(target->fid) == OBJ_TYPE_ITEM && target->owner == nullptr
                && item_get_type(target) == ITEM_TYPE_CONTAINER && !obj_is_locked(target)))
        && actor->elevation == target->elevation
        && actor->tile >= 0 && target->tile >= 0
        && (target->flags & OBJECT_HIDDEN) == 0;
}

bool lootTargetIsInRange(Object* actor, Object* target)
{
    return lootTargetIsValid(actor, target)
        // Native adjacency includes overlapping a large corpse's footprint.
        && obj_dist(actor, target) <= 1;
}

bool isPlayerActor(Object* actor)
{
    std::optional<EntityId> actorId = session.entities().findEntity(actor);
    return actorId.has_value() && session.players().findByActor(*actorId) != nullptr;
}

bool approachInventoryTarget(Object* actor, Object* target, std::uint64_t turnRevision = 0)
{
    if (actor == nullptr || target == nullptr || actor->elevation != target->elevation
        || !hexGridTileIsValid(actor->tile) || !hexGridTileIsValid(target->tile)) return false;
    auto actorId = session.entities().findEntity(actor);
    auto targetId = session.entities().findEntity(target);
    if (!actorId.has_value() || !targetId.has_value() || register_clear(actor) == -2) return false;
    if (obj_dist(actor, target) <= 1) return true;
    if (register_begin(actor == obj_dude ? ANIMATION_REQUEST_RESERVED : ANIMATION_REQUEST_UNRESERVED) == -1)
        return false;
    // Some weapon sets have walking art but no armed running animation.
    int runningFid = art_id(FID_TYPE(actor->fid), actor->fid & 0xFFF, ANIM_RUNNING,
        (actor->fid & 0xF000) >> 12, actor->rotation + 1);
    bool combat = turnRevision != 0;
    int movementAp = combat ? actor->data.critter.combat.ap : -1;
    int result = !combat && obj_dist(actor, target) >= 5 && art_exists(runningFid)
        ? register_object_run_to_object(actor, target, -1, 0)
        : register_object_move_to_object(actor, target, movementAp, 0);
    int committed = register_end();
    if (result == -1 || committed == -1) {
        std::fprintf(stderr, "INVENTORY_APPROACH_REGISTRATION_FAILED actor=%u fid=%d result=%d committed=%d distance=%d\n",
            actorId->value, actor->fid, result, committed, obj_dist(actor, target));
        register_clear(actor);
        return false;
    }
    // Finish host movement before publishing the skill event and checkpoint.
    // Keep the authenticated acting-player context alive while callbacks run.
    combatActionResolving = true;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (session.entities().findObject(*actorId) == actor
        && session.entities().findObject(*targetId) == target
        && anim_busy(actor) == -1 && std::chrono::steady_clock::now() < deadline) {
        process_bk();
        renderPresent();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool present = session.entities().findObject(*actorId) == actor
        && session.entities().findObject(*targetId) == target;
    if (session.entities().findObject(*actorId) == actor && anim_busy(actor) == -1) register_clear(actor);
    combatActionResolving = false;
    if (present && obj_dist(actor, target) > 1)
        std::fprintf(stderr, "INVENTORY_APPROACH_UNREACHABLE actor=%u fid=%d tile=%d target=%d distance=%d\n",
            actorId->value, actor->fid, actor->tile, target->tile, obj_dist(actor, target));
    if (!present) return false;
    bool phaseMatches = combat
        ? session.phase() == SessionPhase::Combat && isInCombat()
            && combatTurns.revision() == turnRevision && networkWorldCombatTurnMatches(actor)
        : session.phase() == SessionPhase::Exploration && !isInCombat();
    return present && phaseMatches
        && critter_is_active(actor) && actor->elevation == target->elevation && obj_dist(actor, target) <= 1;
}

bool finishInteractionAnimation(Object* actor, Object* target)
{
    auto actorId = session.entities().findEntity(actor).value_or(EntityId {});
    auto targetId = session.entities().findEntity(target).value_or(EntityId {});
    combatActionResolving = true;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    auto busy = [&]() {
        return anim_busy(actor) == -1
            || (session.entities().findObject(targetId) == target && anim_busy(target) == -1);
    };
    while (session.entities().findObject(actorId) == actor && busy()
        && std::chrono::steady_clock::now() < deadline) {
        process_bk();
        renderPresent();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool finished = session.entities().findObject(actorId) == actor && !busy();
    if (!finished) {
        if (session.entities().findObject(actorId) == actor) register_clear(actor);
        if (session.entities().findObject(targetId) == target) register_clear(target);
    }
    combatActionResolving = false;
    return finished;
}

bool isAdjacentPlayerActor(Object* actor, Object* target)
{
    // Gifts share the distance rule with loot, but the recipient is alive.
    return actor != nullptr && target != nullptr && actor != target
        && actor->elevation == target->elevation && obj_dist(actor, target) == 1
        && isPlayerActor(target);
}

bool inventoryTransferWouldCycle(Object* destination, Object* item)
{
    for (Object* holder = destination; holder != nullptr; holder = holder->owner) {
        if (holder == item) return true;
    }
    return false;
}

bool remainderIdAvailable(EntityId remainderId)
{
    return !isValid(remainderId) || !session.entities().contains(remainderId);
}

bool applyInventoryTransfer(Object* source,
    Object* destination,
    Object* item,
    std::uint32_t quantity,
    bool force,
    EntityId expectedRemainderId = {},
    bool validateRemainder = false)
{
    if (source == nullptr
        || destination == nullptr
        || item == nullptr
        || source == destination
        || quantity == 0
        || item->owner != source
        || inventoryTransferWouldCycle(destination, item)
        || item_count(source, item) < static_cast<int>(quantity)
        || (validateRemainder
            && (isValid(expectedRemainderId) != (item_count(source, item) > static_cast<int>(quantity))
                || !remainderIdAvailable(expectedRemainderId)))) {
        return false;
    }

    expectedSplitEntityId = expectedRemainderId;
    lastSplitEntityId = {};
    inventoryTransferInProgress = true;
    int rc = force
        ? item_move_force(source, destination, item, static_cast<int>(quantity))
        : item_move(source, destination, item, static_cast<int>(quantity));
    inventoryTransferInProgress = false;
    expectedSplitEntityId = {};
    return rc == 0
        && (!validateRemainder
            || ((isValid(lastSplitEntityId) == isValid(expectedRemainderId))
                && (!isValid(expectedRemainderId) || lastSplitEntityId == expectedRemainderId)));
}

bool applyItemDrop(Object* source,
    Object* item,
    std::uint32_t quantity,
    EntityId expectedRemainderId = {},
    bool validateRemainder = false)
{
    if (source == nullptr
        || item == nullptr
        || quantity == 0
        || quantity > static_cast<std::uint32_t>(item_count(source, item))
        || (quantity > 1 && item->pid != PROTO_ID_MONEY)
        || (validateRemainder
            && (isValid(expectedRemainderId) != (item_count(source, item) > static_cast<int>(quantity))
                || !remainderIdAvailable(expectedRemainderId)))) {
        return false;
    }

    expectedSplitEntityId = expectedRemainderId;
    lastSplitEntityId = {};
    itemDropInProgress = true;
    int rc = quantity == 1
        ? obj_drop(source, item)
        : obj_drop_quantity(source, item, static_cast<int>(quantity));
    itemDropInProgress = false;
    expectedSplitEntityId = {};
    return rc == 0
        && item->owner == nullptr
        && item->tile >= 0
        && elevationIsValid(item->elevation)
        && (!validateRemainder
            || ((isValid(lastSplitEntityId) == isValid(expectedRemainderId))
                && (!isValid(expectedRemainderId) || lastSplitEntityId == expectedRemainderId)));
}

bool setInventoryQuantity(Object* holder, Object* item, std::uint32_t quantity)
{
    if (holder == nullptr || item == nullptr || quantity == 0) {
        return false;
    }
    Inventory* inventory = &holder->data.inventory;
    for (int index = 0; index < inventory->length; index++) {
        InventoryItem* entry = &inventory->items[index];
        if (entry->item == item) {
            entry->quantity = static_cast<int>(quantity);
            return true;
        }
    }
    return false;
}

bool beginPickup(Object* actor, Object* target)
{
    if (isInCombat()
        || actor == nullptr
        || target == nullptr
        || actor == target
        || actor->elevation != target->elevation
        || FID_TYPE(target->fid) != OBJ_TYPE_ITEM
        || target->owner != nullptr) {
        return false;
    }

    std::optional<EntityId> targetId = session.entities().findEntity(target);
    if (!targetId.has_value() || reservedPickupTargets.find(*targetId) != reservedPickupTargets.end()) {
        return false;
    }

    reservedPickupTargets.insert(*targetId);
    if (action_get_an_object(actor, target) == -1) {
        reservedPickupTargets.erase(*targetId);
        return false;
    }
    return true;
}

struct PreservedPeerActor {
    Inventory inventory {};
    CritterObjectData critter {};
    int fid = -1;
};

void attachInventory(Object* actor, Inventory& inventory)
{
    actor->data.inventory = inventory;
    inventory = {};
    for (int index = 0; index < actor->data.inventory.length; index++) {
        actor->data.inventory.items[index].item->owner = actor;
    }
}

bool loadSharedMap(int map, bool snapshotRecovery = false)
{
    if (!session.isActive()
        || (!snapshotRecovery && session.phase() != SessionPhase::Transition)
        || peerActor == nullptr
        || map < 0) {
        return false;
    }

    // Loading destroys the peer object. Map entry scripts must use the native
    // story actor, not the departing guest retained by the command's scope.
    // Otherwise dude_obj can return freed memory while a script grants items.
    auto* nativePlayer = session.players().find(worldMode == NetworkLaunchMode::Host
        ? kHostPlayerId : kGuestPlayerId);
    if (nativePlayer == nullptr || obj_dude == nullptr) return false;
    ScopedActingPlayerContext mapContext(*nativePlayer, obj_dude);

    DetachedActorQueueEvents preservedTimers(peerActor);
    PreservedPeerActor preserved;
    preserved.inventory = peerActor->data.inventory;
    preserved.critter = peerActor->data.critter;
    preserved.critter.combat.whoHitMe = nullptr;
    preserved.fid = peerActor->fid;
    peerActor->data.inventory = {};

    session.clearWorldEntities();
    worldDoors.clear();
    worldScenery.clear();
    worldExitGrids.clear();
    worldItems.clear();
    worldCritters.clear();
    reservedPickupTargets.clear();
    pendingPickups.clear();
    pendingCombatStart.reset();
    deferredEvents.clear();
    dialogueVotes.clear();
    dialoguePresentation.reset();
    pendingTalk.reset();
    dialogueCause = {};
    nextDialogueRevision = 1;
    activeLootTargets.clear();
    theftAccess.clear();
    activeSharedModal.reset();
    approvedWorldMapProposerActorId = {};
    approvedWorldMapProposalSequence = {};
    selectedWorldMapRoute.reset();
    worldMapDeparted = false;
    worldMapOriginX = -1;
    worldMapOriginY = -1;
    pendingWorldMapProposal.reset();
    worldmap_authoritative_travel_cancel();
    peerActor = nullptr;

    if (map_load_idx(map) == -1) {
        obj_inven_free(&preserved.inventory);
        return false;
    }

    Object* replacement = createPeerActor();
    if (replacement == nullptr) {
        obj_inven_free(&preserved.inventory);
        return false;
    }
    replacement->data.critter = preserved.critter;
    attachInventory(replacement, preserved.inventory);
    if (obj_change_fid(replacement, preserved.fid, nullptr) == -1) {
        preserved.inventory = replacement->data.inventory;
        replacement->data.inventory = {};
        obj_inven_free(&preserved.inventory);
        obj_erase_object(replacement, nullptr);
        return false;
    }

    PlayerId peerPlayerId = worldMode == NetworkLaunchMode::Host ? kGuestPlayerId : kHostPlayerId;
    EntityId peerActorId = session.playerActorId(peerPlayerId);
    // Native map teardown really destroyed the old peer body. Its registry
    // entry is now absent, just like its pointer. Restore the known player
    // identity for this map replacement before using the normal rebind path.
    // Respawn would require a distinct body identity and a separate policy.
    if ((session.entities().findObject(peerActorId) == nullptr
            && session.entities().restoreObject(peerActorId, replacement, peerPlayerId)
                != EntityRegistryError::None)
        || session.rebindPlayerActor(peerPlayerId, replacement) != LocalSessionError::None) {
        std::fprintf(stderr, "Multiplayer map replacement could not restore player %u actor %u.\n",
            peerPlayerId.value, peerActorId.value);
        obj_erase_object(replacement, nullptr);
        return false;
    }
    peerActor = replacement;
    preservedTimers.restore(replacement);
    return true;
}

std::optional<WorldMapArrivedEvent> completeWorldMapTravel(WorldMapArrivalKind kind,
    int specialEncounter,
    int forcedMap = -1)
{
    if (worldMode != NetworkLaunchMode::Host
        || !session.isActive()
        || session.phase() != SessionPhase::Transition
        || !activeSharedModal.has_value()
        || activeSharedModal->kind != SharedModalKind::WorldMap) {
        return std::nullopt;
    }

    if (kind == WorldMapArrivalKind::Fatal) {
        WorldMapState position;
        worldmap_capture_state(position);
        EntityId controllerActorId = activeSharedModal->actorId;
        worldmap_authoritative_travel_cancel();
        if (session.transitionTo(SessionPhase::Exploration) != LocalSessionError::None) {
            return std::nullopt;
        }
        activeSharedModal.reset();
        approvedWorldMapProposerActorId = {};
        approvedWorldMapProposalSequence = {};
        selectedWorldMapRoute.reset();
        worldMapDeparted = false;
        worldMapOriginX = -1;
        worldMapOriginY = -1;
        WorldMapArrivedEvent terminal;
        terminal.actorId = controllerActorId;
        terminal.map = map_data.field_34;
        terminal.entranceIndex = 0;
        terminal.phaseRevision = session.phaseRevision();
        terminal.worldX = position.x;
        terminal.worldY = position.y;
        terminal.gameTime = game_time();
        terminal.kind = kind;
        return capturePlayerPlacementRoster(terminal.placements)
            ? std::optional<WorldMapArrivedEvent>(std::move(terminal))
            : std::nullopt;
    }

    int destinationMap = -1;
    int entranceIndex = -1;
    if (forcedMap >= 0) {
        destinationMap = forcedMap;
        entranceIndex = 0;
    } else if (!worldmap_multiplayer_choose_destination(kind == WorldMapArrivalKind::Encounter,
                   specialEncounter,
                   kind == WorldMapArrivalKind::City,
                   &destinationMap,
                   &entranceIndex)) {
        return std::nullopt;
    }
    WorldMapState position;
    worldmap_capture_state(position);
    EntityId controllerActorId = activeSharedModal->actorId;
    int arrivalTime = game_time();
    game_global_vars[GVAR_LOAD_MAP_INDEX] = entranceIndex;
    if (!loadSharedMap(destinationMap)) {
        return std::nullopt;
    }
    Object* hostActor = session.entities().findObject(session.playerActorId(kHostPlayerId));
    if (hostActor == nullptr
        || !placePlayerRosterAtDestination(kHostPlayerId,
            hostActor->tile, hostActor->elevation, hostActor->rotation, true)
        || map_set_elevation(hostActor->elevation) != 0
        || !registerWorldObjects()
        || session.transitionTo(SessionPhase::Exploration) != LocalSessionError::None) {
        return std::nullopt;
    }

    WorldMapArrivedEvent arrival;
    arrival.actorId = controllerActorId;
    arrival.map = destinationMap;
    arrival.entranceIndex = entranceIndex;
    arrival.phaseRevision = session.phaseRevision();
    arrival.worldX = position.x;
    arrival.worldY = position.y;
    arrival.gameTime = arrivalTime;
    arrival.kind = kind;
    return capturePlayerPlacementRoster(arrival.placements)
        ? std::optional<WorldMapArrivedEvent>(std::move(arrival))
        : std::nullopt;
}

// Advance through each due queue boundary on the host only. The replica gets
// the resulting world clock and all rule effects from the authoritative state.
bool sharedPlayersHealed()
{
    for (PlayerId playerId : session.players().playerIds()) {
        Object* player = session.entities().findObject(session.playerActorId(playerId));
        if (player == nullptr || critter_get_hits(player) < stat_level(player, STAT_MAXIMUM_HIT_POINTS)) {
            return false;
        }
    }
    return true;
}

void healRemotePlayersForHours(int hours)
{
    int healingPeriods = hours / 3;
    for (PlayerId playerId : session.players().playerIds()) {
        Object* player = session.entities().findObject(session.playerActorId(playerId));
        if (player != nullptr && player != obj_dude && !isPartyMember(player)) {
            critter_adjust_hits(player, healingPeriods * stat_level(player, STAT_HEALING_RATE));
        }
    }
}

// True means rest stopped before its requested goal, including an interrupting
// queue event or a safety bound. The final clock and all effects still publish.
bool advanceSharedRest(int minutes, bool untilHealed)
{
    if (networkRuntimeSimulationStopped()) return true;
    if (networkWorldPartyDefeated()) return true;
    constexpr int kTicksPerMinute = GAME_TIME_TICKS_PER_HOUR / 60;
    int endTime = game_time() + minutes * kTicksPerMinute;
    int nextHealingTime = game_time() + 3 * GAME_TIME_TICKS_PER_HOUR;
    // Installed maps can schedule a one-second repeating event. A full day
    // therefore needs more than 86,400 boundaries even without other events.
    constexpr int kMaximumRestQueueBoundaries = 3000000;
    int boundaries = 0;
    int stagnantBoundaries = 0;
    bool interrupted = false;
    while (game_time() < endTime
        && (!untilHealed || !sharedPlayersHealed())
        && !networkRuntimeSimulationStopped()
        && boundaries++ < kMaximumRestQueueBoundaries) {
        int nextTime = endTime;
        int nextEventTime = queue_next_time();
        if (nextEventTime > 0 && nextEventTime < nextTime) {
            nextTime = std::max(game_time(), nextEventTime);
        }
        nextTime = std::min(nextTime, nextHealingTime);
        stagnantBoundaries = nextTime == game_time() ? stagnantBoundaries + 1 : 0;
        if (stagnantBoundaries > 128) {
            std::vector<QueueEventState> events;
            queue_capture_state(events);
            std::fprintf(stderr,
                "Multiplayer rest interrupted by a non-advancing queue at time=%d next=%d type=%d.\n",
                game_time(), nextEventTime, events.empty() ? -1 : events.front().eventType);
            interrupted = true;
            break;
        }
        set_game_time(nextTime);
        if (nextEventTime > 0 && nextEventTime <= game_time()) {
            int queueResult = queue_process();
            if (queueResult != 0 || game_user_wants_to_quit != 0
                || networkRuntimeSimulationStopped() || networkWorldPartyDefeated()) {
                std::fprintf(stderr,
                    "Multiplayer rest interrupted by queue at time=%d result=%d quit=%d.\n",
                    game_time(), queueResult, game_user_wants_to_quit);
                interrupted = true;
                break;
            }
        }
        if (networkRuntimeSimulationStopped()) { interrupted = true; break; }
        if (game_time() >= nextHealingTime) {
            partyMemberRestingHeal(3);
            healRemotePlayersForHours(3);
            nextHealingTime += 3 * GAME_TIME_TICKS_PER_HOUR;
        }
    }
    if (boundaries >= kMaximumRestQueueBoundaries) {
        std::fprintf(stderr, "Multiplayer rest stopped at the queue boundary limit, time=%d.\n", game_time());
        interrupted = true;
    }
    return interrupted || networkRuntimeSimulationStopped() || (untilHealed && !sharedPlayersHealed());
}

bool validateDirectTradePlan(const DirectTradeCommitPlan& plan)
{
    for (const DirectTradeLeg& leg : plan.legs) {
        Object* source = session.entities().findObject(leg.sourceActorId);
        Object* destination = session.entities().findObject(
            leg.destinationActorId);
        if (source == nullptr || destination == nullptr
            || session.playerActorId(leg.sourcePlayerId) != leg.sourceActorId
            || session.playerActorId(leg.destinationPlayerId)
                != leg.destinationActorId
            || !isAdjacentPlayerActor(source, destination)
            || leg.offer.caps
                > static_cast<std::uint32_t>(item_caps_total(source))) {
            return false;
        }
        for (const DirectTradeItemOffer& offered : leg.offer.items) {
            Object* item = session.entities().findObject(offered.itemId);
            if (item == nullptr || item->owner != source
                || item->pid == PROTO_ID_MONEY
                || (item->flags & (OBJECT_EQUIPPED | OBJECT_USED)) != 0
                || item_count(source, item) < static_cast<int>(offered.quantity)) {
                return false;
            }
        }
    }
    return true;
}

bool registerUntrackedInventory(Object* owner)
{
    if (owner == nullptr) return false;
    Inventory* inventory = &owner->data.inventory;
    for (int index = 0; index < inventory->length; index++) {
        Object* item = inventory->items[index].item;
        if (!session.entities().findEntity(item).has_value()
            && !registerItem(item)) return false;
        if (!registerUntrackedInventory(item)) return false;
    }
    return true;
}

bool discoverUntrackedWorldObjects()
{
    std::vector<Object*> discovered;
    std::size_t newCritters = 0;
    std::size_t newScenery = 0;
    std::size_t newItems = 0;
    // The native iterator revisits its first occupied tile. Treat the scan
    // as a set so a placed object receives only one snapshot section entry.
    std::unordered_set<Object*> seenObjects;
    for (Object* object = obj_find_first(); object != nullptr; object = obj_find_next()) {
        if (!seenObjects.insert(object).second) continue;
        if (session.entities().findEntity(object).has_value()
            || !hexGridTileIsValid(object->tile) || !elevationIsValid(object->elevation)
            || isExitGrid(object)) continue;
        int type = FID_TYPE(object->fid);
        if (type == OBJ_TYPE_CRITTER && PID_TYPE(object->pid) == OBJ_TYPE_CRITTER) {
            ++newCritters;
        } else if (type == OBJ_TYPE_SCENERY && !obj_is_a_portal(object)) {
            ++newScenery;
        } else if (type == OBJ_TYPE_ITEM && object->owner == nullptr) {
            ++newItems;
        } else {
            continue;
        }
        discovered.push_back(object);
    }
    if (worldCritters.size() + newCritters > kMaxSnapshotCritters)
        return captureFailure(SnapshotCaptureFailure::SnapshotValidation, "discovery", {}, SnapshotError::TooManyCritters);
    if (worldScenery.size() + newScenery > kMaxSnapshotScenery)
        return captureFailure(SnapshotCaptureFailure::SnapshotValidation, "discovery", {}, SnapshotError::TooManyScenery);
    if (worldItems.size() + newItems > kMaxSnapshotItems)
        return captureFailure(SnapshotCaptureFailure::SnapshotValidation, "discovery", {}, SnapshotError::TooManyItems);
    std::sort(discovered.begin(), discovered.end(), [](const Object* lhs, const Object* rhs) {
        return std::tie(lhs->elevation, lhs->tile, lhs->pid, lhs->id, lhs->fid)
            < std::tie(rhs->elevation, rhs->tile, rhs->pid, rhs->id, rhs->fid);
    });
    for (Object* object : discovered) {
        EntityRegistrationResult registration = session.registerWorldObject(object);
        if (!registration) return captureFailure(SnapshotCaptureFailure::WorldRegistration, "discovery");
        switch (FID_TYPE(object->fid)) {
        case OBJ_TYPE_CRITTER:
            worldCritters.emplace_back(registration.entityId, object);
            break;
        case OBJ_TYPE_SCENERY:
            worldScenery.emplace_back(registration.entityId, object);
            break;
        case OBJ_TYPE_ITEM:
            trackWorldItem(registration.entityId, object);
            break;
        }
    }
    return true;
}

bool npcBarterItemAvailable(Object* owner, Object* item, bool seller)
{
    if (owner == nullptr || item == nullptr || item->owner != owner || item->pid == PROTO_ID_MONEY
        || (item->flags & (OBJECT_EQUIPPED | OBJECT_USED)) != 0 || item_queued(item)) return false;
    if (seller && inven_right_hand(owner) == nullptr && item == inven_find_type(owner, ITEM_TYPE_WEAPON, nullptr)) return false;
    return true;
}

bool validateNpcBarterOffer(Object* owner, const DirectTradeOffer& offer, bool seller)
{
    if (!isValidBarterOffer(offer) || offer.caps > static_cast<unsigned>(std::max(0, item_caps_total(owner)))) return false;
    for (const auto& entry : offer.items) {
        Object* item = session.entities().findObject(entry.itemId);
        if (!npcBarterItemAvailable(owner, item, seller)
            || entry.quantity > static_cast<unsigned>(item_count(owner, item))) return false;
    }
    return true;
}

std::uint32_t npcBarterOfferValue(const DirectTradeOffer& offer)
{
    std::int64_t value = offer.caps;
    for (const auto& entry : offer.items) {
        Object* item = session.entities().findObject(entry.itemId);
        if (item == nullptr) return 0;
        if (item_get_type(item) == ITEM_TYPE_AMMO) {
            Proto* proto = nullptr;
            if (proto_ptr(item->pid, &proto) != 0) return 0;
            value += static_cast<std::int64_t>(proto->item.cost) * (entry.quantity - 1) + item_cost(item);
        } else value += static_cast<std::int64_t>(item_cost(item)) * entry.quantity;
    }
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(value, 0, INT32_MAX));
}

void priceNpcBarter(Object* buyer, Object* seller, NpcBarterState& state)
{
    int modifier = 0;
    switch (reaction_to_level(reaction_get(seller))) {
    case NPC_REACTION_BAD: modifier = -25; break;
    case NPC_REACTION_GOOD: modifier = 50; break;
    default: break;
    }
    int rate = std::clamp(100 + 25 * (perk_level(PERK_MASTER_TRADER) != 0)
        + skill_level(buyer, SKILL_BARTER) - skill_level(seller, SKILL_BARTER)
        + gdialogGetBarterModifier() + modifier, 10, 300);
    state.offeredValue = npcBarterOfferValue(state.buyerOffer);
    auto goods = std::max<std::int64_t>(0, static_cast<std::int64_t>(npcBarterOfferValue(state.sellerOffer)) - state.sellerOffer.caps);
    state.askingValue = static_cast<std::uint32_t>(std::min<std::int64_t>(INT32_MAX,
        static_cast<std::int64_t>(goods) * 100 / rate + state.sellerOffer.caps));
}

bool applyDirectTradePlan(const DirectTradeCommitPlan& plan, bool npcBarter = false)
{
    if (!npcBarter && !validateDirectTradePlan(plan)) return false;
    for (std::size_t index = 0; index < plan.legs.size(); ++index) {
        Object* source = session.entities().findObject(plan.legs[index].sourceActorId);
        if (source == nullptr) return false;
        auto finalCaps = static_cast<std::int64_t>(item_caps_total(source))
            - plan.legs[index].offer.caps + plan.legs[1 - index].offer.caps;
        if (finalCaps < 0 || finalCaps > INT32_MAX) return false;
    }
    struct AppliedItem {
        Object* source = nullptr;
        Object* destination = nullptr;
        Object* item = nullptr;
        std::uint32_t quantity = 0;
    };
    std::vector<AppliedItem> applied;
    // Reserve before detaching anything, so delivery and rollback cannot fail
    // because an inventory needs to grow. Detach both offers before inserting
    // either: a stack merge destroys the destination's old item object/ID.
    for (const DirectTradeLeg& leg : plan.legs) {
        Object* destination = session.entities().findObject(leg.destinationActorId);
        Inventory& inventory = destination->data.inventory;
        int capacity = inventory.length + static_cast<int>(leg.offer.items.size()) + 1;
        if (inventory.capacity < capacity) {
            auto* items = static_cast<InventoryItem*>(mem_realloc(inventory.items,
                sizeof(InventoryItem) * capacity));
            if (items == nullptr) return false;
            inventory.items = items;
            inventory.capacity = capacity;
        }
    }
    auto restoreDetached = [&]() {
        for (auto rollback = applied.rbegin(); rollback != applied.rend(); ++rollback) {
            item_add_force(rollback->source, rollback->item,
                static_cast<int>(rollback->quantity));
        }
    };
    for (const DirectTradeLeg& leg : plan.legs) {
        Object* source = session.entities().findObject(leg.sourceActorId);
        Object* destination = session.entities().findObject(leg.destinationActorId);
        for (const DirectTradeItemOffer& offered : leg.offer.items) {
            Object* item = session.entities().findObject(offered.itemId);
            if (item_remove_mult(source, item, static_cast<int>(offered.quantity)) != 0) {
                restoreDetached();
                return false;
            }
            applied.push_back({ source, destination, item, offered.quantity });
        }
    }

    std::array<int, 2> capDeltas {};
    capDeltas[0] = static_cast<int>(plan.legs[1].offer.caps)
        - static_cast<int>(plan.legs[0].offer.caps);
    capDeltas[1] = -capDeltas[0];
    std::size_t adjusted = 0;
    for (; adjusted < plan.legs.size(); adjusted++) {
        Object* actor = session.entities().findObject(
            plan.legs[adjusted].sourceActorId);
        if (capDeltas[adjusted] != 0
            && item_caps_adjust(actor, capDeltas[adjusted]) != 0) break;
    }
    if (adjusted != plan.legs.size()) {
        while (adjusted > 0) {
            adjusted--;
            Object* actor = session.entities().findObject(
                plan.legs[adjusted].sourceActorId);
            if (capDeltas[adjusted] != 0) {
                item_caps_adjust(actor, -capDeltas[adjusted]);
            }
        }
        restoreDetached();
        return false;
    }
    // Register split remainders and newly created cap stacks before delivery.
    // After delivery a merge can destroy the previous representative, so a
    // registration failure must still have a complete rollback path here.
    if (!registerUntrackedInventory(session.entities().findObject(plan.legs[0].sourceActorId))
        || !registerUntrackedInventory(session.entities().findObject(plan.legs[1].sourceActorId))) {
        for (std::size_t index = 0; index < plan.legs.size(); ++index) {
            if (capDeltas[index] != 0) item_caps_adjust(
                session.entities().findObject(plan.legs[index].sourceActorId), -capDeltas[index]);
        }
        restoreDetached();
        return false;
    }
    for (const AppliedItem& transfer : applied) {
        item_add_force(transfer.destination, transfer.item,
            static_cast<int>(transfer.quantity));
    }
    return true;
}

class NetworkCommandExecutor : public CommandExecutor {
public:
    bool actorCanExecute(Object* actor, const GameCommand& command) const override
    {
        // Closing a window and yielding a turn must remain possible after an
        // injury. Neither grants an incapacitated actor a new world action.
        if (std::holds_alternative<EndTurnCommand>(command.payload)) return true;
        if (const auto* equipment = std::get_if<EquipmentCommand>(&command.payload);
            equipment != nullptr && equipment->action == EquipmentAction::CloseInventory) return true;
        if (const auto* modal = std::get_if<SharedModalCommand>(&command.payload);
            modal != nullptr && !modal->open) return true;
        if (const auto* barter = std::get_if<NpcBarterCommand>(&command.payload);
            barter != nullptr && barter->action == NpcBarterAction::Cancel) return true;
        if (const auto* trade = std::get_if<DirectTradeCommand>(&command.payload);
            trade != nullptr && trade->action == DirectTradeAction::Cancel) return true;
        return !storyPresentation.active && actor != nullptr && !critter_is_dead(actor)
            && (actor->data.critter.combat.results & DAM_KNOCKED_OUT) == 0
            && !networkWorldPartyDefeated();
    }

    CommandExecutionStatus startCombat(Object* actor, Object* target, const StartCombatCommand& command) override
    {
        auto actorId = session.entities().findEntity(actor);
        if (worldMode != NetworkLaunchMode::Host || session.phase() != SessionPhase::Exploration
            || isInCombat() || pendingCombatStart.has_value() || !isValid(command)
            || !actorId.has_value() || !critter_is_active(actor)) return CommandExecutionStatus::InvalidAction;
        if (isValid(command.targetId) && (target == nullptr || target == actor
            || FID_TYPE(target->fid) != OBJ_TYPE_CRITTER || !critter_is_active(target)
            || target->elevation != actor->elevation
            || combat_check_bad_shot(actor, target, command.hitMode, command.hitLocation != HIT_LOCATION_UNCALLED)
                != COMBAT_BAD_SHOT_OK)) return CommandExecutionStatus::InvalidAction;
        pendingCombatStart = PendingCombatStart { *actorId, command };
        return CommandExecutionStatus::Applied;
    }

    CommandExecutionStatus advanceCharacter(Object* actor, const CharacterAdvanceCommand& command) override
    {
        auto* player = actingPlayerState();
        if (!isValid(command) || player == nullptr || actingPlayerActor() != actor
            || characterAdvancementFingerprint(player->build) != command.expectedBuild) {
            return CommandExecutionStatus::InvalidAction;
        }
        PlayerCharacterState draft = *player;
        {
            ScopedActingPlayerContext context(draft, actor);
            if (command.perk != -1) {
                int selected = std::count_if(draft.build.perkRanks.begin(), draft.build.perkRanks.end(),
                    [](int rank) { return rank > 0; });
                if (draft.build.pendingPerks <= 0 || selected >= 7) return CommandExecutionStatus::InvalidAction;
                if (command.perk == PERK_TAG) {
                    if (command.taggedSkill < 0 || draft.build.taggedSkills[3] != -1
                        || std::find(draft.build.taggedSkills.begin(), draft.build.taggedSkills.end(), command.taggedSkill)
                            != draft.build.taggedSkills.end()) return CommandExecutionStatus::InvalidAction;
                } else if (command.taggedSkill != -1) return CommandExecutionStatus::InvalidAction;
                if (command.perk == PERK_MUTATE) {
                    bool hasTraits = draft.build.traits[0] != -1;
                    auto removed = std::find(draft.build.traits.begin(), draft.build.traits.end(), command.removedTrait);
                    if (command.addedTrait < 0 || (hasTraits && (command.removedTrait < 0 || removed == draft.build.traits.end()))
                        || (!hasTraits && command.removedTrait != -1)) return CommandExecutionStatus::InvalidAction;
                    if (hasTraits) *removed = -1;
                    if (std::find(draft.build.traits.begin(), draft.build.traits.end(), command.addedTrait)
                        != draft.build.traits.end()) return CommandExecutionStatus::InvalidAction;
                    if (draft.build.traits[0] == -1) std::swap(draft.build.traits[0], draft.build.traits[1]);
                    draft.build.traits[hasTraits && draft.build.traits[0] != -1 ? 1 : 0] = command.addedTrait;
                } else if (command.removedTrait != -1 || command.addedTrait != -1) {
                    return CommandExecutionStatus::InvalidAction;
                }
                // Native prerequisite checks must run before Mutate changes
                // the traits used to qualify for the selected perk.
                if (command.perk == PERK_MUTATE) {
                    auto newTraits = draft.build.traits;
                    draft.build.traits = player->build.traits;
                    if (perk_add(command.perk) != 0) return CommandExecutionStatus::InvalidAction;
                    draft.build.traits = newTraits;
                } else if (perk_add(command.perk) != 0) return CommandExecutionStatus::InvalidAction;
                if (command.perk == PERK_TAG) draft.build.taggedSkills[3] = command.taggedSkill;
                if (command.perk == PERK_LIFEGIVER) {
                    stat_set_bonus(actor, STAT_MAXIMUM_HIT_POINTS, stat_get_bonus(actor, STAT_MAXIMUM_HIT_POINTS) + 4);
                }
                if (command.perk == PERK_EDUCATED) {
                    stat_pc_set(PC_STAT_UNSPENT_SKILL_POINTS, stat_pc_get(PC_STAT_UNSPENT_SKILL_POINTS) + 2);
                }
                --draft.build.pendingPerks;
                stat_recalc_derived(actor);
            } else if (command.taggedSkill != -1 || command.removedTrait != -1 || command.addedTrait != -1) {
                return CommandExecutionStatus::InvalidAction;
            }
            for (int skill = 0; skill < SKILL_COUNT; ++skill) {
                for (int point = 0; point < command.skillIncrements[skill]; ++point) {
                    if (skill_inc_point(actor, skill) != 0) return CommandExecutionStatus::InvalidAction;
                }
            }
            draft.build.prototypeFlags &= ~(1U << PC_FLAG_LEVEL_UP_AVAILABLE);
        }
        player->build = draft.build;
        if (command.perk == PERK_LIFEGIVER) critter_adjust_hits(actor, 4);
        return CommandExecutionStatus::Applied;
    }
    bool movementRunning(Object* actor, bool requested) override
    {
        return requested && actor != nullptr
            && (actor->data.critter.combat.results & DAM_CRIP_LEG_ANY) == 0
            && art_exists(art_id(FID_TYPE(actor->fid), actor->fid & 0xFFF, ANIM_RUNNING, 0, actor->rotation + 1));
    }

    bool combatActionAllowed(Object* actor, std::uint64_t revision) const
    {
        if (worldMode != NetworkLaunchMode::Host || !session.isActive()
            || session.phase() != SessionPhase::Combat || !isInCombat()
            || actor == nullptr || revision == 0 || combatActionResolving
            || (actor->data.critter.combat.results
                & (DAM_KNOCKED_OUT | DAM_DEAD | DAM_LOSE_TURN)) != 0) {
            return false;
        }
        std::optional<EntityId> actorId = session.entities().findEntity(actor);
        const CombatTurnEntry* turn = combatTurns.current();
        return actorId.has_value() && turn != nullptr
            && turn->actorId == *actorId && turn->owner.has_value()
            && turn->owner == networkWorldCombatOwner(actor)
            && combatTurns.revision() == revision;
    }

    CommandExecutionStatus combatMove(Object* actor,
        const CombatMoveCommand& command) override
    {
        if (!combatActionAllowed(actor, command.turnRevision)
            || !hexGridTileIsValid(command.destinationTile)
            || command.elevation != actor->elevation
            || command.destinationTile == actor->tile
            || actor->data.critter.combat.ap + combat_free_move <= 0) {
            return CommandExecutionStatus::InvalidAction;
        }
        std::array<unsigned char, kMaximumMovementPathLength> path;
        int pathLength = make_path(actor, actor->tile,
            command.destinationTile, path.data(), 1);
        if (pathLength <= 0 || pathLength > kAnimationMaximumPathLength) {
            return CommandExecutionStatus::InvalidAction;
        }
        int startingTile = actor->tile;
        register_clear(actor);
        int request = actor == obj_dude
            ? ANIMATION_REQUEST_RESERVED : ANIMATION_REQUEST_UNRESERVED;
        if (register_begin(request) == -1) {
            return CommandExecutionStatus::InvalidAction;
        }
        int scheduled = register_object_move_along_path(actor,
            command.destinationTile, command.elevation, path.data(),
            pathLength, movementRunning(actor, command.running), 0);
        int committed = register_end();
        if (scheduled == -1 || committed == -1) {
            register_clear(actor);
            return CommandExecutionStatus::InvalidAction;
        }
        if (command.running && !perk_level(PERK_SILENT_RUNNING)) pc_flag_off(PC_FLAG_SNEAKING);
        combatActionResolving = true;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (anim_busy(actor) == -1 && std::chrono::steady_clock::now() < deadline) {
            process_bk();
            renderPresent();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (anim_busy(actor) == -1) {
            register_clear(actor);
        }
        combatActionResolving = false;
        return actor->tile != startingTile
            ? CommandExecutionStatus::Applied : CommandExecutionStatus::InvalidAction;
    }

    CommandExecutionStatus combatItem(Object* actor,
        const CombatItemCommand& command) override
    {
        Object* item = session.entities().findObject(command.itemId);
        Object* target = isValid(command.targetId)
            ? session.entities().findObject(command.targetId) : nullptr;
        if (!combatActionAllowed(actor, command.turnRevision)
            || item == nullptr || item->owner != actor
            || (isValid(command.targetId)
                ? target == nullptr || target == item
                    || target->elevation != actor->elevation
                    || target->tile < 0
                    || obj_dist(actor, target) > 1
                    || !proto_action_can_use_on(item->pid)
                : !proto_action_can_use(item->pid)
                    && !proto_action_can_use_on(item->pid))
            || actor->data.critter.combat.ap < 2) {
            return CommandExecutionStatus::InvalidAction;
        }
        bool explosive = obj_is_explosive(item);
        if (!isValidExplosiveTimerChoice(command.timerSeconds)
            || (explosive ? command.timerSeconds == 0 || target != nullptr : command.timerSeconds != 0))
            return CommandExecutionStatus::InvalidAction;
        combatActionResolving = true;
        int result = explosive ? obj_arm_explosive(actor, item, command.timerSeconds)
            : target != nullptr
            ? obj_use_item_on(actor, target, item)
            : proto_action_can_use_on(item->pid)
                ? obj_use_item_on(actor, actor, item)
                : obj_use_item(actor, item);
        combatActionResolving = false;
        if (result != 0) {
            return CommandExecutionStatus::InvalidAction;
        }
        actor->data.critter.combat.ap -= 2;
        if (actor == obj_dude) {
            intface_update_items(false);
            intface_update_move_points(actor->data.critter.combat.ap, combat_free_move);
        }
        return CommandExecutionStatus::Applied;
    }

    CommandExecutionStatus combatReload(Object* actor,
        const CombatReloadCommand& command) override
    {
        Object* weapon = session.entities().findObject(command.weaponId);
        if (!combatActionAllowed(actor, command.turnRevision)
            || weapon == nullptr || weapon->owner != actor
            || item_get_type(weapon) != ITEM_TYPE_WEAPON
            || (command.hitMode == HIT_MODE_LEFT_WEAPON_RELOAD
                && inven_left_hand(actor) != weapon)
            || (command.hitMode == HIT_MODE_RIGHT_WEAPON_RELOAD
                && inven_right_hand(actor) != weapon)
            || (command.hitMode != HIT_MODE_LEFT_WEAPON_RELOAD
                && command.hitMode != HIT_MODE_RIGHT_WEAPON_RELOAD)
            || actor->data.critter.combat.ap
                < item_mp_cost(actor, command.hitMode, false)) {
            return CommandExecutionStatus::InvalidAction;
        }
        bool reloaded = false;
        combatActionResolving = true;
        while (item_w_try_reload(actor, weapon) != -1) {
            reloaded = true;
        }
        combatActionResolving = false;
        if (!reloaded) {
            return CommandExecutionStatus::InvalidAction;
        }
        actor->data.critter.combat.ap -= item_mp_cost(actor, command.hitMode, false);
        if (actor == obj_dude) {
            intface_update_items(false);
            intface_update_move_points(actor->data.critter.combat.ap, combat_free_move);
        }
        return CommandExecutionStatus::Applied;
    }

    CommandExecutionStatus combatFace(Object* actor,
        const CombatFaceCommand& command) override
    {
        if (!combatActionAllowed(actor, command.turnRevision)
            || command.rotation < 0 || command.rotation >= ROTATION_COUNT
            || command.rotation == actor->rotation) {
            return CommandExecutionStatus::InvalidAction;
        }
        Rect dirtyRect {};
        if (obj_set_rotation(actor, command.rotation, &dirtyRect) == -1) {
            return CommandExecutionStatus::InvalidAction;
        }
        tile_refresh_rect(&dirtyRect, actor->elevation);
        return CommandExecutionStatus::Applied;
    }

    CommandExecutionStatus move(Object* actor, const MoveCommand& command) override
    {
        if (isInCombat()
            || !hexGridTileIsValid(command.destinationTile)
            || !elevationIsValid(command.elevation)
            || command.elevation != actor->elevation
            || command.destinationTile == actor->tile) {
            return CommandExecutionStatus::InvalidAction;
        }

        std::array<unsigned char, kMaximumMovementPathLength> path;
        int pathLength = make_path(actor, actor->tile, command.destinationTile, path.data(), 1);
        if (pathLength <= 0 || pathLength > kAnimationMaximumPathLength) {
            return CommandExecutionStatus::InvalidAction;
        }

        register_clear(actor);
        int requestOptions = actor == obj_dude
            ? ANIMATION_REQUEST_RESERVED
            : ANIMATION_REQUEST_UNRESERVED;
        if (register_begin(requestOptions) == -1) {
            return CommandExecutionStatus::InvalidAction;
        }
        int rc = register_object_move_along_path(actor,
            command.destinationTile,
            command.elevation,
            path.data(),
            pathLength,
            command.running,
            0);
        int endRc = register_end();
        if (rc != -1 && endRc != -1 && command.running && !perk_level(PERK_SILENT_RUNNING)) pc_flag_off(PC_FLAG_SNEAKING);
        return rc != -1 && endRc != -1
            ? CommandExecutionStatus::Applied
            : CommandExecutionStatus::InvalidAction;
    }

    CommandExecutionStatus face(Object* actor, const FaceCommand& command) override
    {
        if (command.rotation < 0 || command.rotation >= ROTATION_COUNT) {
            return CommandExecutionStatus::InvalidAction;
        }
        Rect dirtyRect;
        if (obj_set_rotation(actor, command.rotation, &dirtyRect) == -1) {
            return CommandExecutionStatus::InvalidAction;
        }
        tile_refresh_rect(&dirtyRect, actor->elevation);
        return CommandExecutionStatus::Applied;
    }

    bool interactionAllowed(Object* actor, std::uint64_t turnRevision) const
    {
        return actor != nullptr && critter_is_active(actor) && !combatActionResolving
            && (turnRevision != 0 ? combatActionAllowed(actor, turnRevision)
                : worldMode == NetworkLaunchMode::Host && !isInCombat()
                    && session.phase() == SessionPhase::Exploration);
    }

    bool combatLootAccess(Object* actor, std::uint64_t revision) const
    {
        auto actorId = session.entities().findEntity(actor).value_or(EntityId {});
        auto access = openLootTurns.find(actorId);
        auto target = activeLootTargets.find(actor);
        return revision != 0 && access != openLootTurns.end() && access->second == revision
            && target != activeLootTargets.end() && session.entities().findEntity(target->second).has_value()
            && lootTargetIsInRange(actor, target->second);
    }

    DoorUseExecution useDoor(Object* actor, Object* target, std::uint64_t turnRevision = 0) override
    {
        DoorUseExecution execution;
        if (!interactionAllowed(actor, turnRevision) || target == nullptr
            || actor->elevation != target->elevation || !obj_is_a_portal(target)
            || !approachInventoryTarget(actor, target, turnRevision)) return execution;
        auto actorId = session.entities().findEntity(actor).value_or(EntityId {});
        auto targetId = session.entities().findEntity(target).value_or(EntityId {});
        if ((turnRevision != 0 && actor->data.critter.combat.ap < 3)
            || action_use_an_object(actor, target) == -1
            || !finishInteractionAnimation(actor, target)
            || session.entities().findObject(actorId) != actor
            || session.entities().findObject(targetId) != target
            || !interactionAllowed(actor, turnRevision)) return execution;
        execution.status = CommandExecutionStatus::Applied;
        execution.open = obj_is_open(target) != 0;
        execution.locked = obj_is_locked(target);
        execution.frame = target->frame;
        return execution;
    }

    CommandExecutionStatus pickup(Object* actor, Object* target, std::uint64_t turnRevision = 0) override
    {
        if (turnRevision == 0) return interactionAllowed(actor, 0) && beginPickup(actor, target)
            ? CommandExecutionStatus::Applied : CommandExecutionStatus::InvalidAction;
        if (!interactionAllowed(actor, turnRevision) || target == nullptr
            || FID_TYPE(target->fid) != OBJ_TYPE_ITEM || target->owner != nullptr
            || actor->elevation != target->elevation
            || (item_get_type(target) == ITEM_TYPE_CONTAINER && !proto_action_can_pickup(target->pid)))
            return CommandExecutionStatus::InvalidAction;
        auto actorId = session.entities().findEntity(actor).value_or(EntityId {});
        auto targetId = session.entities().findEntity(target).value_or(EntityId {});
        if (!isValid(targetId) || reservedPickupTargets.count(targetId) != 0
            || !approachInventoryTarget(actor, target, turnRevision)
            || actor->data.critter.combat.ap < 3) return CommandExecutionStatus::InvalidAction;
        reservedPickupTargets.insert(targetId);
        int started = action_get_an_object(actor, target);
        bool finished = started != -1 && finishInteractionAnimation(actor, target);
        reservedPickupTargets.erase(targetId);
        return finished && session.entities().findObject(actorId) == actor
            && interactionAllowed(actor, turnRevision)
            ? CommandExecutionStatus::Applied : CommandExecutionStatus::InvalidAction;
    }

    CommandExecutionStatus loot(Object* actor, Object* target, std::uint64_t turnRevision = 0, bool targetChange = false) override
    {
        if (!interactionAllowed(actor, turnRevision) || isPlayerActor(target)
            || !lootTargetIsValid(actor, target)) {
            return CommandExecutionStatus::InvalidAction;
        }
        auto actorId = session.entities().findEntity(actor).value_or(EntityId {});
        auto targetId = session.entities().findEntity(target).value_or(EntityId {});
        if (targetChange) {
            auto previous = activeLootTargets.find(actor);
            if (previous == activeLootTargets.end() || !session.entities().findEntity(previous->second).has_value()
                || !lootTargetIsInRange(actor, previous->second) || !lootTargetIsInRange(actor, target)
                || FID_TYPE(target->fid) != OBJ_TYPE_CRITTER || target->tile != previous->second->tile
                || (turnRevision != 0 && !combatLootAccess(actor, turnRevision)))
                return CommandExecutionStatus::InvalidAction;
            if (!registerUntrackedInventory(target)) return CommandExecutionStatus::InvalidAction;
            activeLootTargets[actor] = target;
            return CommandExecutionStatus::Applied;
        }
        activeLootTargets.erase(actor);
        openLootTurns.erase(actorId);
        if (!approachInventoryTarget(actor, target, turnRevision)) return CommandExecutionStatus::InvalidAction;
        auto current = [&]() {
            return session.entities().findObject(actorId) == actor
                && session.entities().findObject(targetId) == target
                && (turnRevision != 0
                    ? session.phase() == SessionPhase::Combat && isInCombat()
                        && combatTurns.revision() == turnRevision && networkWorldCombatTurnMatches(actor)
                    : session.phase() == SessionPhase::Exploration && !isInCombat())
                && critter_is_active(actor) && !isPlayerActor(target)
                && lootTargetIsInRange(actor, target);
        };
        if (!current() || check_scenery_ap_cost(actor, target) == -1) return CommandExecutionStatus::InvalidAction;
        if (FID_TYPE(target->fid) == OBJ_TYPE_ITEM && target->frame == 0 && target->pid != 213) {
            combatActionResolving = true;
            int result = obj_use_container(actor, target);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (result == 0 && current() && anim_busy(target) == -1
                && std::chrono::steady_clock::now() < deadline) {
                process_bk();
                renderPresent();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            bool finished = current() && anim_busy(target) != -1;
            if (current() && !finished) register_clear(target);
            combatActionResolving = false;
            if (result == -1 || !finished) return CommandExecutionStatus::InvalidAction;
            int sid = -1;
            Script* script = nullptr;
            if (obj_sid(target, &sid) != -1
                && (scr_ptr(sid, &script) == -1 || script->scriptOverrides)) {
                return CommandExecutionStatus::InvalidAction;
            }
        }
        int sid = -1;
        if (obj_sid(target, &sid) != -1) {
            scr_set_objs(sid, actor, nullptr);
            exec_script_proc(sid, SCRIPT_PROC_PICKUP);
            Script* script = nullptr;
            if (!current() || scr_ptr(sid, &script) == -1 || script->scriptOverrides)
                return CommandExecutionStatus::InvalidAction;
        }
        if (!current() || !registerUntrackedInventory(target)) return CommandExecutionStatus::InvalidAction;
        activeLootTargets[actor] = target;
        if (turnRevision != 0) openLootTurns[actorId] = turnRevision;
        return CommandExecutionStatus::Applied;
    }

    CommandExecutionStatus useSkill(Object* actor, Object* target, const UseSkillCommand& command) override
    {
        if (command.skill == ExplorationSkill::Steal) {
            if (worldMode != NetworkLaunchMode::Host || isInCombat()
                || combatActionResolving
                || session.phase() != SessionPhase::Exploration || command.turnRevision != 0
                || actor == nullptr || target == nullptr || actor == target || isPlayerActor(target)
                || !critter_is_active(actor)
                || actor->elevation != target->elevation || actor->tile < 0 || target->tile < 0
                || theftAccess.count(actor) != 0
                || std::any_of(theftAccess.begin(), theftAccess.end(),
                    [target](const auto& entry) { return entry.second.target == target; }))
                return CommandExecutionStatus::InvalidAction;
            bool livingTarget = FID_TYPE(target->fid) == OBJ_TYPE_CRITTER && critter_is_active(target);
            bool lootTarget = (FID_TYPE(target->fid) == OBJ_TYPE_CRITTER && !critter_is_active(target))
                || (FID_TYPE(target->fid) == OBJ_TYPE_ITEM && target->owner == nullptr
                    && item_get_type(target) == ITEM_TYPE_CONTAINER && !obj_is_locked(target));
            if ((!livingTarget && !lootTarget) || !approachInventoryTarget(actor, target)
                || !registerUntrackedInventory(target)) return CommandExecutionStatus::InvalidAction;
            auto actorId = session.entities().findEntity(actor).value_or(EntityId {});
            auto targetId = session.entities().findEntity(target).value_or(EntityId {});
            activeLootTargets.erase(actor);
            int scriptResult = obj_use_skill_script(actor, target, SKILL_STEAL);
            // Scripts may move, destroy, or replace either object. Resolve IDs
            // before dereferencing the old pointers or granting inventory access.
            if (scriptResult == -1) return CommandExecutionStatus::InvalidAction;
            if (scriptResult == 1) return CommandExecutionStatus::Applied;
            if (session.entities().findObject(actorId) != actor
                || session.entities().findObject(targetId) != target
                || isInCombat() || session.phase() != SessionPhase::Exploration
                || !critter_is_active(actor) || actor->elevation != target->elevation
                || actor->tile < 0 || target->tile < 0 || obj_dist(actor, target) > 1
                || (target->flags & OBJECT_HIDDEN) != 0 || isPlayerActor(target)
                || !registerUntrackedInventory(target))
                return CommandExecutionStatus::InvalidAction;
            // A target can fall unconscious while the actor approaches it.
            if (FID_TYPE(target->fid) != OBJ_TYPE_CRITTER || !critter_is_active(target)) {
                CommandExecutionStatus result = loot(actor, target);
                if (result == CommandExecutionStatus::Applied && actor == localPlayerActor())
                    scripts_request_loot_container(actor, target);
                return result;
            }
            theftAccess.emplace(actor, TheftAccess { target });
            activeLootTargets[actor] = target;
            if (actor == localPlayerActor()) scripts_request_steal_container(actor, target);
            return CommandExecutionStatus::Applied;
        }
        if (command.skill == ExplorationSkill::Sneak) {
            if (worldMode != NetworkLaunchMode::Host || actor == nullptr || actor != target
                || (actor->data.critter.combat.results & (DAM_DEAD | DAM_KNOCKED_OUT)) != 0
                || (isInCombat() ? !combatActionAllowed(actor, command.turnRevision)
                    : session.phase() != SessionPhase::Exploration || command.turnRevision != 0)) {
                return CommandExecutionStatus::InvalidAction;
            }
            register_clear(actor);
            pc_flag_toggle(PC_FLAG_SNEAKING);
            return CommandExecutionStatus::Applied;
        }
        if (isInCombat()
            || command.turnRevision != 0
            || actor == nullptr
            || target == nullptr
            || actor->elevation != target->elevation
            || target->tile < 0
            || !isValid(command.skill)
            || action_use_skill_on(actor, target, static_cast<int>(command.skill)) == -1) {
            return CommandExecutionStatus::InvalidAction;
        }
        return CommandExecutionStatus::Applied;
    }

    bool skillInventoryOpened(Object* actor, Object* target) const override
    {
        auto found = activeLootTargets.find(actor);
        return found != activeLootTargets.end() && found->second == target;
    }

    CommandExecutionStatus useItemOn(Object* actor,
        Object* item,
        Object* target,
        const UseItemOnCommand&) override
    {
        if (isInCombat()
            || actor == nullptr
            || item == nullptr
            || target == nullptr
            || item == target
            || actor->elevation != target->elevation
            || target->tile < 0
            || FID_TYPE(item->fid) != OBJ_TYPE_ITEM
            || topEnvironmentOrSelf(item) != actor
            || anim_busy(actor) == -1) {
            return CommandExecutionStatus::InvalidAction;
        }
        itemUseInProgress = true;
        int rc = action_use_an_item_on_object(actor, target, item);
        itemUseInProgress = false;
        return rc != -1
            ? CommandExecutionStatus::Applied
            : CommandExecutionStatus::InvalidAction;
    }

    ElevatorExecution useElevator(Object* actor, const ElevatorCommand& command) override
    {
        ElevatorExecution execution;
        Object* host = session.entities().findObject(session.playerActorId(kHostPlayerId));
        Object* guest = session.entities().findObject(session.playerActorId(kGuestPlayerId));
        std::optional<EntityId> actingActorId = session.entities().findEntity(actor);
        PlayerCharacterState* actingPlayer = actingActorId.has_value()
            ? session.players().findByActor(*actingActorId)
            : nullptr;
        int sourceTile = -1;
        if (isInCombat()
            || actor == nullptr
            || host == nullptr
            || guest == nullptr
            || actingPlayer == nullptr
            || session.phase() != SessionPhase::Exploration
            || !elevator_get_source(command.elevatorType, map_data.field_34, actor->elevation, &sourceTile)
            || tile_dist(actor->tile, sourceTile) > 4) {
            return execution;
        }

        int destinationMap = -1;
        int destinationElevation = -1;
        int destinationTile = -1;
        if (!elevator_get_destination(command.elevatorType,
                command.destinationLevel,
                &destinationMap,
                &destinationElevation,
                &destinationTile)
            || !elevationIsValid(destinationElevation)
            || !hexGridTileIsValid(destinationTile)
            || (destinationMap == map_data.field_34 && destinationElevation == actor->elevation)
            || (destinationMap != map_data.field_34
                && !allConnectedPlayersNear(sourceTile, actor->elevation, 4))) {
            return execution;
        }

        if (destinationMap != map_data.field_34) {
            if (session.transitionTo(SessionPhase::Transition) != LocalSessionError::None
                || !loadSharedMap(destinationMap)) {
                return execution;
            }
            host = session.entities().findObject(session.playerActorId(kHostPlayerId));
            guest = session.entities().findObject(session.playerActorId(kGuestPlayerId));
            bool loaded = map_data.field_34 == destinationMap
                && placePlayerRosterAtDestination(kHostPlayerId,
                    destinationTile, destinationElevation, ROTATION_SE);
            Object* localActor = localPlayerActor();
            loaded = loaded
                && localActor != nullptr
                && map_set_elevation(localActor->elevation) == 0
                && registerWorldObjects()
                && session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None;
            if (!loaded) {
                return execution;
            }

            execution.map = destinationMap;
            if (!fillElevatorExecutionFromRoster(execution)) return ElevatorExecution {};
            execution.status = CommandExecutionStatus::Applied;
            execution.phaseRevision = session.phaseRevision();
            return execution;
        }

        int oldHostTile = host->tile;
        int oldHostElevation = host->elevation;
        int oldHostRotation = host->rotation;
        int oldGuestTile = guest->tile;
        int oldGuestElevation = guest->elevation;
        int oldGuestRotation = guest->rotation;
        int oldMapElevation = map_elevation;
        std::vector<PlayerId> riders;
        bool placed = placeReadyElevatorRiders(actingPlayer->id,
            sourceTile, actor->elevation, destinationTile, destinationElevation, riders);
        Object* localActor = localPlayerActor();
        int localElevation = localActor != nullptr ? localActor->elevation : oldMapElevation;
        placed = placed && (localElevation == map_elevation || map_set_elevation(localElevation) == 0);
        if (!placed
            || session.transitionTo(SessionPhase::Transition) != LocalSessionError::None
            || session.transitionTo(SessionPhase::Exploration) != LocalSessionError::None) {
            obj_move_to_tile(host, oldHostTile, oldHostElevation, nullptr);
            obj_move_to_tile(guest, oldGuestTile, oldGuestElevation, nullptr);
            obj_set_rotation(host, oldHostRotation, nullptr);
            obj_set_rotation(guest, oldGuestRotation, nullptr);
            map_set_elevation(oldMapElevation);
            session.transitionTo(SessionPhase::Exploration);
            return execution;
        }
        for (PlayerId playerId : riders) {
            Object* rider = session.entities().findObject(session.playerActorId(playerId));
            if (rider == nullptr || obj_set_rotation(rider, ROTATION_SE, nullptr) == -1) {
                return execution;
            }
        }

        execution.map = destinationMap;
        if (!fillElevatorExecutionFromRoster(execution)) return ElevatorExecution {};
        execution.status = CommandExecutionStatus::Applied;
        execution.phaseRevision = session.phaseRevision();
        return execution;
    }

    ExitGridExecution useExitGrid(Object* actor, Object* target, const ExitGridCommand&) override
    {
        ExitGridExecution execution;
        PlayerCharacterState* actingPlayer = session.players().findByActor(
            session.entities().findEntity(actor).value_or(EntityId {}));
        auto roster = playerActorRoster();
        int destinationMap = -1;
        int destinationTile = -1;
        int destinationElevation = -1;
        int destinationRotation = -1;
        if (isInCombat()
            || actingPlayer == nullptr
            || !roster.has_value()
            || target == nullptr
            || actor->tile != target->tile
            || actor->elevation != target->elevation
            || !allConnectedPlayersNear(target->tile, target->elevation, 4)
            || !exitGridDestination(target,
                destinationMap,
                destinationTile,
                destinationElevation,
                destinationRotation)
            || session.phase() != SessionPhase::Exploration) {
            return execution;
        }

        if (isWorldMapDestination(destinationMap)) {
            SharedModalExecution proposal = setSharedModal(actor,
                SharedModalCommand { SharedModalKind::WorldMap, true });
            if (proposal.status == CommandExecutionStatus::Applied
                && pendingWorldMapProposal.has_value()
                && pendingWorldMapProposal->proposerActorId == session.playerActorId(actingPlayer->id)) {
                pendingWorldMapProposal->sourceEntityId = session.entities().findEntity(target).value_or(EntityId {});
                pendingWorldMapProposal->sourceTile = target->tile;
                pendingWorldMapProposal->sourceElevation = target->elevation;
                pendingWorldMapProposal->proposerMustStandOnSource = true;
            }
            execution.status = proposal.status;
            execution.phaseRevision = proposal.phaseRevision;
            execution.proposedWorldMap = proposal.status == CommandExecutionStatus::Applied;
            return execution;
        }

        if (session.transitionTo(SessionPhase::Transition) != LocalSessionError::None
            || !loadSharedMap(destinationMap)) {
            return execution;
        }

        bool loaded = map_data.field_34 == destinationMap
            && placePlayerRosterAtDestination(actingPlayer->id,
                destinationTile, destinationElevation, destinationRotation);
        Object* localActor = localPlayerActor();
        loaded = loaded
            && localActor != nullptr
            && map_set_elevation(localActor->elevation) == 0
            && registerWorldObjects()
            && session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None;
        if (!loaded) {
            return execution;
        }

        execution.status = CommandExecutionStatus::Applied;
        execution.map = destinationMap;
        if (!capturePlayerPlacementRoster(execution.placements)) return ExitGridExecution {};
        execution.phaseRevision = session.phaseRevision();
        return execution;
    }

    SceneryTransitionExecution useSceneryTransition(Object* actor,
        Object* target,
        const SceneryTransitionCommand&) override
    {
        SceneryTransitionExecution execution;
        PlayerCharacterState* actingPlayer = session.players().findByActor(
            session.entities().findEntity(actor).value_or(EntityId {}));
        std::vector<PlayerTransitionPlacement> previous;
        int sceneryType = -1;
        if (isInCombat()
            || actor == nullptr
            || actingPlayer == nullptr
            || !capturePlayerPlacementRoster(previous)
            || !sceneryTransitionType(target, sceneryType)
            || actor->elevation != target->elevation
            || tile_dist(actor->tile, target->tile) > 4
            || session.phase() != SessionPhase::Exploration) {
            return execution;
        }
        bool companionReady = allConnectedPlayersNear(target->tile, target->elevation, 4);
        int sourceMap = map_data.field_34;
        int declaredDestinationMap = sceneryType == SCENERY_TYPE_STAIRS
            ? target->data.scenery.stairs.destinationMap
            : sourceMap;
        if (declaredDestinationMap > 0
            && declaredDestinationMap != sourceMap
            && !companionReady) {
            return execution;
        }
        if (sceneryType == SCENERY_TYPE_STAIRS
            && !companionReady
            && !typedStairCanBeUsedIndependently(target, sourceMap)) {
            return execution;
        }

        int oldMapElevation = map_elevation;
        capturedSceneryMapTransition.reset();
        scriptedSceneryTransitionInProgress = true;
        int useResult = obj_use(actor, target);
        scriptedSceneryTransitionInProgress = false;
        std::optional<MapTransition> requestedTransition = capturedSceneryMapTransition;
        capturedSceneryMapTransition.reset();
        bool requestedWorldMap = scripts_take_worldmap_request();
        if (useResult == -1) {
            return execution;
        }

        if (requestedWorldMap || (requestedTransition.has_value() && isWorldMapDestination(requestedTransition->map))) {
            if (!companionReady) return execution;
            SharedModalExecution proposal = setSharedModal(actor,
                SharedModalCommand { SharedModalKind::WorldMap, true });
            if (proposal.status == CommandExecutionStatus::Applied
                && pendingWorldMapProposal.has_value()
                && pendingWorldMapProposal->proposerActorId == session.playerActorId(actingPlayer->id)) {
                pendingWorldMapProposal->sourceEntityId = session.entities().findEntity(target).value_or(EntityId {});
                pendingWorldMapProposal->sourceTile = target->tile;
                pendingWorldMapProposal->sourceElevation = target->elevation;
            }
            execution.status = proposal.status;
            execution.phaseRevision = proposal.phaseRevision;
            execution.proposedWorldMap = proposal.status == CommandExecutionStatus::Applied;
            return execution;
        }

        if (requestedTransition.has_value()) {
            if (requestedTransition->map <= 0
                || requestedTransition->map == sourceMap
                || !companionReady
                || session.transitionTo(SessionPhase::Transition) != LocalSessionError::None
                || !loadSharedMap(requestedTransition->map)) {
                return execution;
            }
            Object* host = session.entities().findObject(session.playerActorId(kHostPlayerId));
            int destinationTile = requestedTransition->tile;
            int destinationElevation = requestedTransition->elevation;
            int destinationRotation = requestedTransition->rotation;
            if (!hexGridTileIsValid(destinationTile) || !elevationIsValid(destinationElevation)) {
                destinationTile = host != nullptr ? host->tile : -1;
                destinationElevation = host != nullptr ? host->elevation : -1;
            }
            if (destinationRotation < 0 || destinationRotation >= ROTATION_COUNT) {
                destinationRotation = host != nullptr ? host->rotation : ROTATION_SE;
            }
            bool loaded = map_data.field_34 == requestedTransition->map
                && placePlayerRosterAtDestination(actingPlayer->id,
                    destinationTile, destinationElevation, destinationRotation);
            Object* localActor = localPlayerActor();
            loaded = loaded
                && localActor != nullptr
                && map_set_elevation(localActor->elevation) == 0
                && registerWorldObjects()
                && session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None;
            if (!loaded) {
                return execution;
            }
            execution.map = requestedTransition->map;
        } else {
            bool companionsRestored = true;
            bool moved = false;
            for (const PlayerTransitionPlacement& placement : previous) {
                Object* participant = session.entities().findObject(placement.actorId);
                if (participant == nullptr) {
                    companionsRestored = false;
                    break;
                }
                if (placement.playerId == actingPlayer->id) {
                    moved = participant->tile != placement.tile
                        || participant->elevation != placement.elevation;
                } else if (obj_move_to_tile(participant,
                               placement.tile, placement.elevation, nullptr) == -1
                    || obj_set_rotation(participant, placement.rotation, nullptr) == -1) {
                    companionsRestored = false;
                    break;
                }
            }
            Object* localActor = localPlayerActor();
            bool applied = companionsRestored
                && moved
                && localActor != nullptr
                && (localActor->elevation == map_elevation || map_set_elevation(localActor->elevation) == 0)
                && session.transitionTo(SessionPhase::Transition) == LocalSessionError::None
                && session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None;
            if (!applied) {
                applyPlayerPlacementRoster(previous);
                map_set_elevation(oldMapElevation);
                if (session.phase() == SessionPhase::Transition) {
                    session.transitionTo(SessionPhase::Exploration);
                }
                return execution;
            }
            execution.map = sourceMap;
        }

        execution.status = CommandExecutionStatus::Applied;
        if (!capturePlayerPlacementRoster(execution.placements)) return SceneryTransitionExecution {};
        execution.phaseRevision = session.phaseRevision();
        return execution;
    }

    RestExecution rest(Object* actor, const RestCommand& command) override
    {
        RestExecution execution;
        std::optional<EntityId> actorId = session.entities().findEntity(actor);
        PlayerCharacterState* player = actorId.has_value()
            ? session.players().findByActor(*actorId)
            : nullptr;
        if (actor == nullptr
            || player == nullptr
            || isInCombat()
            || activeSharedModal.has_value()
            || session.phase() != SessionPhase::Exploration
            || !isValidRestMinutes(command.minutes)) {
            return execution;
        }

        auto now = std::chrono::steady_clock::now();
        if (pendingRestProposal.has_value()
            && (pendingRestProposal->expiresAt <= now
                || pendingRestProposal->map != map_data.field_34
                || pendingRestProposal->phaseRevision != session.phaseRevision())) {
            pendingRestProposal.reset();
        }
        if (command.minutes == 0) {
            if (!pendingRestProposal.has_value()
                || pendingRestProposal->readyPlayers.erase(player->id) == 0) {
                return execution;
            }
            if (pendingRestProposal->proposer == player->id
                || pendingRestProposal->readyPlayers.empty()) {
                pendingRestProposal.reset();
            }
        } else {
            int requestedMinutes = command.minutes > 0 ? command.minutes
                : command.minutes == kRestUntilHealed ? 30 * 24 * 60
                : restMinutesUntilHour(command.minutes, game_time_hour());
            if (requestedMinutes <= 0
                || game_time() <= 0
                || game_time() > std::numeric_limits<int>::max()
                        - requestedMinutes * (GAME_TIME_TICKS_PER_HOUR / 60)) {
                return execution;
            }
            for (PlayerId participantId : session.players().playerIds()) {
                Object* participant = session.entities().findObject(session.playerActorId(participantId));
                if (participant == nullptr || !critter_can_actor_rest(participant)
                    || (command.minutes == kRestUntilHealed
                        && critter_get_hits(participant) < stat_level(participant, STAT_MAXIMUM_HIT_POINTS)
                        && stat_level(participant, STAT_HEALING_RATE) <= 0)) {
                    return execution;
                }
            }
            if (!pendingRestProposal.has_value()
                || pendingRestProposal->minutes != command.minutes) {
                pendingRestProposal = PendingRestProposal {
                    command.minutes,
                    player->id,
                    map_data.field_34,
                    session.phaseRevision(),
                    now + kRestProposalLifetime,
                    {},
                };
            }
            pendingRestProposal->readyPlayers.insert(player->id);
            pendingRestProposal->expiresAt = now + kRestProposalLifetime;
            if (allConnectedPlayersReady(pendingRestProposal->readyPlayers)) {
                if (session.transitionTo(SessionPhase::Transition) != LocalSessionError::None) {
                    return execution;
                }
                pendingRestProposal.reset();
                // This duration was resolved from the current host clock for
                // this approval, not stored with the older proposal.
                execution.interrupted = advanceSharedRest(requestedMinutes,
                    command.minutes == kRestUntilHealed);
                if (execution.interrupted) {
                    char message[] = "Shared rest was interrupted.";
                    display_print(message);
                }
                if (session.transitionTo(SessionPhase::Exploration) != LocalSessionError::None) {
                    return execution;
                }
                execution.completed = true;
            }
        }
        execution.status = CommandExecutionStatus::Applied;
        execution.gameTime = game_time();
        execution.phaseRevision = session.phaseRevision();
        return execution;
    }

    CommandExecutionStatus attack(Object* actor, Object* target, const AttackCommand& command) override
    {
        std::optional<EntityId> actorId = actor != nullptr
            ? session.entities().findEntity(actor) : std::nullopt;
        const CombatTurnEntry* turn = combatTurns.current();
        if (worldMode != NetworkLaunchMode::Host
            || session.phase() != SessionPhase::Combat
            || !isInCombat()
            || actor == nullptr
            || target == nullptr
            || actor == target
            || actor->elevation != target->elevation
            || FID_TYPE(target->fid) != OBJ_TYPE_CRITTER
            || command.hitMode < 0 || command.hitMode >= HIT_MODE_COUNT
            || command.hitMode == HIT_MODE_LEFT_WEAPON_RELOAD
            || command.hitMode == HIT_MODE_RIGHT_WEAPON_RELOAD
            || command.hitLocation < 0 || command.hitLocation >= HIT_LOCATION_COUNT
            || !actorId.has_value()
            || turn == nullptr
            || turn->actorId != *actorId
            || !turn->owner.has_value()
            || turn->owner != networkWorldCombatOwner(actor)
            || combatTurns.revision() != command.turnRevision
            || (actor->data.critter.combat.results
                & (DAM_KNOCKED_OUT | DAM_DEAD | DAM_LOSE_TURN)) != 0
            || combatActionResolving) {
            return CommandExecutionStatus::InvalidAction;
        }
        int badShot = combat_check_bad_shot(actor, target, command.hitMode,
            command.hitLocation != HIT_LOCATION_UNCALLED);
        if (badShot != COMBAT_BAD_SHOT_OK) {
            std::fprintf(stderr, "Attack rejected: actor=%u target=%u mode=%d location=%d reason=%d ap=%d required=%d distance=%d.\n",
                actorId->value, command.targetId.value, command.hitMode, command.hitLocation,
                badShot, actor->data.critter.combat.ap,
                item_w_mp_cost(actor, command.hitMode, command.hitLocation != HIT_LOCATION_UNCALLED), obj_dist(actor, target));
            return CommandExecutionStatus::InvalidAction;
        }
        Object* weapon = item_hit_with(actor, command.hitMode);
        int weaponArt = weapon != nullptr && item_get_type(weapon) == ITEM_TYPE_WEAPON
            ? item_w_anim_code(weapon) : 0;
        int attackFid = art_id(OBJ_TYPE_CRITTER, actor->fid & 0xFFF,
            item_w_anim(actor, command.hitMode), weaponArt, actor->rotation + 1);
        if (!art_exists(attackFid) || register_clear(actor) == -2) {
            return CommandExecutionStatus::InvalidAction;
        }
        Rect bounds;
        int readyFid = art_id(OBJ_TYPE_CRITTER, actor->fid & 0xFFF,
            ANIM_STAND, weaponArt, actor->rotation + 1);
        if (obj_change_fid(actor, readyFid, &bounds) == -1) return CommandExecutionStatus::InvalidAction;
        tile_refresh_rect(&bounds, actor->elevation);
        combatActionResolving = true;
        int result = combat_attack(actor, target, command.hitMode, command.hitLocation);
        if (result == 0) {
            // Fallout finishes ammo, damage, death, scripts, and XP in an
            // animation callback. Keep the acting-player context alive until
            // that callback has completed before publishing the result.
            combat_turn_run();
        }
        combatActionResolving = false;
        return result == 0
            ? CommandExecutionStatus::Applied
            : CommandExecutionStatus::InvalidAction;
    }

    CommandExecutionStatus inventoryAction(Object* actor, const InventoryActionCommand& command) override
    {
        Object* item = session.entities().findObject(command.itemId);
        if (worldMode != NetworkLaunchMode::Host || actor == nullptr
            || combatActionResolving || !isValid(command)
            || item == nullptr || topEnvironmentOrSelf(item) != actor
            || FID_TYPE(item->fid) != OBJ_TYPE_ITEM || item->owner == nullptr || item_count(item->owner, item) < 1
            || (actor->data.critter.combat.results & (DAM_DEAD | DAM_KNOCKED_OUT)) != 0) {
            return CommandExecutionStatus::InvalidAction;
        }
        Object* holder = item->owner;
        // Equipment belongs to the actor itself, never an owned container.
        if (holder != actor && (item->flags & OBJECT_EQUIPPED) != 0)
            return CommandExecutionStatus::InvalidAction;
        bool combat = session.phase() == SessionPhase::Combat;
        auto actorId = session.entities().findEntity(actor).value_or(EntityId {});
        auto access = openInventories.find(actorId);
        // Native inventory use, unload and ammo dragging cost only the opening
        // charge. A command cannot claim that charge was paid without an open
        // inventory for this exact combat turn.
        if (combat ? !combatActionAllowed(actor, command.turnRevision)
                || (command.action != InventoryAction::Scan
                    && (access == openInventories.end() || access->second != command.turnRevision)
                    && !combatLootAccess(actor, command.turnRevision))
            : session.phase() != SessionPhase::Exploration || command.turnRevision != 0) {
            return CommandExecutionStatus::InvalidAction;
        }
        if (command.action == InventoryAction::Scan) {
            if (item->pid != PROTO_ID_MOTION_SENSOR || holder != actor
                || (inven_left_hand(actor) != item && inven_right_hand(actor) != item))
                return CommandExecutionStatus::InvalidAction;
            return item_m_use_motion_sensor(item) == 0 && registerUntrackedInventory(actor)
                ? CommandExecutionStatus::Applied : CommandExecutionStatus::InvalidAction;
        }
        if (command.action == InventoryAction::Drop) {
            if (item_count(holder, item) < static_cast<int>(command.quantity)
                || !hexGridTileIsValid(actor->tile) || !elevationIsValid(actor->elevation)) {
                return CommandExecutionStatus::InvalidAction;
            }
            ScopedLocalPlayerBinding binding(actor);
            Object* armor = inven_worn(actor);
            bool armorDropped = item == armor;
            bool activeWeaponDropped = (item->flags & (OBJECT_IN_LEFT_HAND | OBJECT_IN_RIGHT_HAND)) != 0
                && item_get_type(item) == ITEM_TYPE_WEAPON
                && item_w_anim_code(item) == ((actor->fid >> 12) & 0xF);
            bool caps = item->pid == PROTO_ID_MONEY;
            std::uint32_t count = caps ? 1 : command.quantity;
            std::uint32_t dropped = 0;
            for (; dropped < count; ++dropped) {
                if (item == nullptr || !applyItemDrop(holder, item, caps ? command.quantity : 1)) break;
                if (caps) item_caps_set_amount(item, command.quantity);
                // The original stack object is on the ground. Its split copy
                // remains in inventory with the identity registered by native removal.
                item = session.entities().findObject(lastSplitEntityId);
            }
            if (dropped == 0) return CommandExecutionStatus::InvalidAction;
            if (armorDropped) adjust_ac(actor, armor, nullptr);
            int gender = stat_level(actor, STAT_GENDER) == GENDER_FEMALE ? GENDER_FEMALE : GENDER_MALE;
            int baseArt = art_vault_person_nums[gender];
            if (Object* remainingArmor = inven_worn(actor)) {
                int armorArt = gender == GENDER_FEMALE
                    ? item_ar_female_fid(remainingArmor) : item_ar_male_fid(remainingArmor);
                if (armorArt != -1) baseArt = armorArt;
            }
            int weaponArt = activeWeaponDropped ? 0 : (actor->fid >> 12) & 0xF;
            Rect bounds;
            if (obj_change_fid(actor, art_id(OBJ_TYPE_CRITTER, baseArt, ANIM_STAND,
                    weaponArt, actor->rotation + 1), &bounds) == 0) {
                tile_refresh_rect(&bounds, actor->elevation);
            }
            return registerUntrackedInventory(actor)
                ? CommandExecutionStatus::Applied : CommandExecutionStatus::InvalidAction;
        }
        if (command.action == InventoryAction::Use) {
            bool explosive = obj_is_explosive(item);
            if (explosive ? command.timerSeconds == 0 : command.timerSeconds != 0)
                return CommandExecutionStatus::InvalidAction;
            if (item_get_type(item) == ITEM_TYPE_CONTAINER
                || (!proto_action_can_use(item->pid) && !proto_action_can_use_on(item->pid))) {
                return CommandExecutionStatus::InvalidAction;
            }
            itemUseInProgress = true;
            int result;
            if (explosive) {
                result = obj_arm_explosive(actor, item, command.timerSeconds);
            } else if (item_get_type(item) == ITEM_TYPE_DRUG) {
                // Native inventory drugs apply directly, without target-use
                // scripts or the HUD's additional combat AP charge.
                result = item_d_take_drug(actor, item) ? 0 : -1;
                if (result == 0) obj_destroy(item);
            } else {
                result = proto_action_can_use(item->pid)
                    ? obj_use_item(actor, item) : obj_use_item_on(actor, actor, item);
            }
            itemUseInProgress = false;
            return result == 0 && registerUntrackedInventory(actor)
                ? CommandExecutionStatus::Applied : CommandExecutionStatus::InvalidAction;
        }
        if (item_get_type(item) != ITEM_TYPE_WEAPON) return CommandExecutionStatus::InvalidAction;
        Object* ammo = isValid(command.ammoId) ? session.entities().findObject(command.ammoId) : nullptr;
        if (command.action == InventoryAction::Reload) {
            if (item_w_curr_ammo(item) >= item_w_max_ammo(item)
                || (isValid(command.ammoId) && (ammo == nullptr || topEnvironmentOrSelf(ammo) != actor
                    || item_get_type(ammo) != ITEM_TYPE_AMMO
                    || item_count(ammo->owner, ammo) < static_cast<int>(command.quantity)
                    || !item_w_can_reload(item, ammo)))) {
                return CommandExecutionStatus::InvalidAction;
            }
        } else if (!item_w_can_unload(item)) {
            return CommandExecutionStatus::InvalidAction;
        }
        // A list entry may represent multiple identical loaded weapons. Detach
        // one before editing its ammo, as the native inventory does.
        int equippedFlags = item->flags & OBJECT_EQUIPPED;
        if (item_remove_mult(holder, item, 1) != 0) return CommandExecutionStatus::InvalidAction;
        bool changed = false;
        if (command.action == InventoryAction::Unload) {
            while (Object* unloaded = item_w_unload(item)) {
                obj_disconnect(unloaded, nullptr);
                item_add_force(holder, unloaded, 1);
                changed = true;
            }
        } else if (ammo == nullptr) {
            while (item_w_try_reload(holder, item) != -1) changed = true;
        } else {
            int ammoPid = ammo->pid;
            Object* ammoHolder = ammo->owner;
            for (std::uint32_t index = 0; index < command.quantity; ++index) {
                if (item_w_curr_ammo(item) >= item_w_max_ammo(item)) break;
                if (ammo == nullptr || item_remove_mult(ammoHolder, ammo, 1) != 0) break;
                int result = item_w_reload(item, ammo);
                if (result == 0) obj_destroy(ammo);
                else item_add_force(ammoHolder, ammo, 1);
                if (result == -1) break;
                changed = true;
                ammo = nullptr;
                for (int slot = 0; slot < ammoHolder->data.inventory.length; ++slot) {
                    Object* candidate = ammoHolder->data.inventory.items[slot].item;
                    if (candidate->pid == ammoPid) { ammo = candidate; break; }
                }
            }
        }
        item->flags |= equippedFlags;
        item_add_force(holder, item, 1);
        return registerUntrackedInventory(actor) && changed
            ? CommandExecutionStatus::Applied : CommandExecutionStatus::InvalidAction;
    }

    CommandExecutionStatus setEquipment(Object* actor, const EquipmentCommand& command) override
    {
        auto reject = [&](const char* reason) {
            std::fprintf(stderr, "Equipment rejected: %s actor=%u phase=%d ap=%d hand=%d left=%u right=%u armor=%u.\n",
                reason, session.entities().findEntity(actor).value_or(EntityId {}).value,
                static_cast<int>(session.phase()), actor != nullptr ? actor->data.critter.combat.ap : -1,
                command.activeHand, command.leftHand.value, command.rightHand.value, command.armor.value);
            char message[160];
            std::snprintf(message, sizeof(message), "Equipment: %s", reason);
            if (actor == localPlayerActor()) display_print(message);
            return CommandExecutionStatus::InvalidAction;
        };
        if (worldMode == NetworkLaunchMode::Host && actor != nullptr
            && command.action == EquipmentAction::CloseInventory) {
            auto id = session.entities().findEntity(actor);
            if (!id.has_value()) return reject("actor is unregistered");
            auto access = openInventories.find(*id);
            if (access != openInventories.end() && access->second != command.turnRevision)
                return reject("inventory access has changed");
            openInventories.erase(*id);
            openLootTurns.erase(*id);
            activeLootTargets.erase(actor);
            return CommandExecutionStatus::Applied;
        }
        if (worldMode != NetworkLaunchMode::Host || actor == nullptr
            || combatActionResolving
            || (actor->data.critter.combat.results & (DAM_DEAD | DAM_KNOCKED_OUT)) != 0
            || (session.phase() != SessionPhase::Exploration
                && session.phase() != SessionPhase::Combat)) {
            return reject("actor is unavailable");
        }
        bool combat = session.phase() == SessionPhase::Combat;
        if ((combat && (!networkWorldCombatTurnMatches(actor)
                    || command.turnRevision != combatTurns.revision()))
            || (!combat && command.turnRevision != 0)) {
            return reject("combat turn has changed");
        }
        // Binding is needed by the legacy armor/perk helpers, which resolve
        // the inventory player through local-player context.
        ScopedLocalPlayerBinding binding(actor);
        if (command.activeHand < 0 || command.activeHand > 1) return reject("invalid active hand");
        EntityId actorId = session.entities().findEntity(actor).value_or(EntityId {});
        auto access = openInventories.find(actorId);
        bool inventoryOpen = access != openInventories.end() && access->second == command.turnRevision;
        if (command.action == EquipmentAction::CloseInventory) {
            openInventories.erase(actorId);
            openLootTurns.erase(actorId);
            activeLootTargets.erase(actor);
            return CommandExecutionStatus::Applied;
        }
        if (command.action == EquipmentAction::OpenInventory) {
            int cost = combat && !inventoryOpen ? std::max(0, 4 - perk_level(PERK_QUICK_POCKETS)) : 0;
            if (actor->data.critter.combat.ap < cost) return reject("not enough action points");
            actor->data.critter.combat.ap -= cost;
            openInventories[actorId] = command.turnRevision;
            return CommandExecutionStatus::Applied;
        }
        if (command.action != EquipmentAction::Set) return reject("invalid inventory action");
        Object* selected[3] {};
        EntityId ids[] = { command.leftHand, command.rightHand, command.armor };
        for (int slot = 0; slot < 3; ++slot) {
            if (!isValid(ids[slot])) continue;
            selected[slot] = session.entities().findObject(ids[slot]);
            Object* item = selected[slot];
            if (item == nullptr || item->owner != actor
                || FID_TYPE(item->fid) != OBJ_TYPE_ITEM
                || item_count(actor, item) < 1
                || (slot == 2 && item_get_type(item) != ITEM_TYPE_ARMOR)) {
                return reject("item is not owned or suitable armor");
            }
            for (int previous = 0; previous < slot; ++previous) {
                if (selected[previous] == item) return reject("one item cannot occupy two slots");
            }
        }
        Object* oldArmor = inven_worn(actor);
        bool changed = selected[0] != inven_left_hand(actor)
            || selected[1] != inven_right_hand(actor) || selected[2] != oldArmor;
        int cost = combat && changed && !inventoryOpen && !combatLootAccess(actor, command.turnRevision) ? std::max(0, 4 - perk_level(PERK_QUICK_POCKETS)) : 0;
        if (cost > 0 && actor->data.critter.combat.ap < cost) return reject("not enough action points");
        int gender = stat_level(actor, STAT_GENDER) == GENDER_FEMALE ? GENDER_FEMALE : GENDER_MALE;
        int baseArt = art_vault_person_nums[gender];
        if (selected[2] != nullptr) {
            int armorArt = gender == GENDER_FEMALE
                ? item_ar_female_fid(selected[2]) : item_ar_male_fid(selected[2]);
            if (armorArt != -1) baseArt = armorArt;
        }
        Object* activeWeapon = selected[command.activeHand];
        int weaponArt = activeWeapon != nullptr && item_get_type(activeWeapon) == ITEM_TYPE_WEAPON
            ? item_w_anim_code(activeWeapon) : 0;
        int fid = art_id(OBJ_TYPE_CRITTER, baseArt, ANIM_STAND, weaponArt, actor->rotation + 1);
        if (!art_exists(fid) || register_clear(actor) == -2) return reject("weapon appearance is unavailable or actor is busy");
        // The ordinary inventory removes one item from a stack before placing
        // it in a hand. Use the same split operation and keep its shared ID.
        for (Object* item : selected) {
            if (item != nullptr && item_count(actor, item) > 1) {
                if (item_remove_mult(actor, item, 1) != 0) return reject("could not split item stack");
                item->flags |= OBJECT_IN_LEFT_HAND; // Prevent immediate restacking.
                if (item_add_force(actor, item, 1) != 0) return reject("could not return split item");
            }
        }
        Rect bounds;
        if (obj_change_fid(actor, fid, &bounds) == -1) return reject("could not draw the equipped item");
        for (int index = 0; index < actor->data.inventory.length; ++index) {
            actor->data.inventory.items[index].item->flags &= ~OBJECT_EQUIPPED;
        }
        if (selected[0] != nullptr) selected[0]->flags |= OBJECT_IN_LEFT_HAND;
        if (selected[1] != nullptr) selected[1]->flags |= OBJECT_IN_RIGHT_HAND;
        if (selected[2] != nullptr) selected[2]->flags |= OBJECT_WORN;
        if (oldArmor != selected[2]) adjust_ac(actor, oldArmor, selected[2]);
        actor->data.critter.combat.ap -= cost;
        auto* player = playerStateForActor(actor);
        if (player != nullptr) player->build.activeHand = command.activeHand;
        tile_refresh_rect(&bounds, actor->elevation);
        return CommandExecutionStatus::Applied;
    }

    EndTurnExecution endTurn(Object* actor, PlayerId playerId,
        const EndTurnCommand& command) override
    {
        EndTurnExecution execution;
        if (worldMode != NetworkLaunchMode::Host
            || session.phase() != SessionPhase::Combat
            || actor == nullptr) {
            return execution;
        }
        std::optional<EntityId> actorId = session.entities().findEntity(actor);
        if (!actorId.has_value()
            || combatTurns.endPlayerTurn(playerId, *actorId,
                   command.turnRevision, combatClockMilliseconds())
                != CombatTurnResult::Accepted) {
            return execution;
        }
        execution.status = CommandExecutionStatus::Applied;
        execution.state = combatTurns.snapshot(combatClockMilliseconds());
        return execution;
    }

    CommandExecutionStatus requestTalk(Object* actor, Object* target,
        const TalkCommand& command) override
    {
        if (worldMode != NetworkLaunchMode::Host || actor == nullptr
            || target == nullptr || target->sid == -1 || isInCombat()
            || FID_TYPE(target->fid) != OBJ_TYPE_CRITTER
            || actor->elevation != target->elevation || obj_dist(actor, target) >= 9
            || activeSharedModal.has_value() || pendingTalk.has_value()
            || session.phase() != SessionPhase::Exploration) {
            return CommandExecutionStatus::InvalidAction;
        }
        // Order another conversation after this NPC's native reaction. This
        // leaves the existing phase and shared modal untouched on rejection.
        Script* script = nullptr;
        if (scr_ptr(target->sid, &script) == 0 && script->program != nullptr
            && intExtraHasDialogueActor(script->program)) {
            return CommandExecutionStatus::InvalidAction;
        }
        std::optional<EntityId> actorId = session.entities().findEntity(actor);
        if (!actorId.has_value()
            || session.transitionTo(SessionPhase::Dialogue) != LocalSessionError::None) {
            return CommandExecutionStatus::InvalidAction;
        }
        activeSharedModal = ActiveSharedModal { *actorId, SharedModalKind::Dialogue };
        pendingTalk = PendingTalk { *actorId, command.targetId, {} };
        dialogueVotes.clear();
        dialoguePresentation.reset();
        return CommandExecutionStatus::Applied;
    }

    CommandExecutionStatus dialogueVote(Object* actor, PlayerId playerId,
        const DialogueVoteCommand& command) override
    {
        if (worldMode != NetworkLaunchMode::Host || actor == nullptr
            || session.phase() != SessionPhase::Dialogue
            || !activeSharedModal.has_value()
            || activeSharedModal->kind != SharedModalKind::Dialogue
            || npcBarterState.has_value()
            || !dialogueVotes.vote(playerId, command.revision, command.option)) {
            return CommandExecutionStatus::InvalidAction;
        }
        return CommandExecutionStatus::Applied;
    }

    SharedModalExecution setSharedModal(Object* actor, const SharedModalCommand& command) override
    {
        SharedModalExecution execution;
        std::optional<EntityId> actorId = session.entities().findEntity(actor);
        if (!actorId.has_value() || !isValid(command.kind)) {
            return execution;
        }

        execution.actorId = *actorId;
        if (command.kind == SharedModalKind::Theft) {
            if (command.open || session.phase() != SessionPhase::Exploration
                || theftAccess.count(actor) == 0) return execution;
            finishTheft(actor, false);
            execution.status = CommandExecutionStatus::Applied;
            execution.phase = session.phase();
            execution.phaseRevision = session.phaseRevision();
            return execution;
        }
        if (command.kind == SharedModalKind::WorldMap) {
            PlayerCharacterState* player = session.players().findByActor(*actorId);
            if (player == nullptr || isInCombat()) {
                return execution;
            }
            const auto now = std::chrono::steady_clock::now();
            if (command.open) {
                if (activeSharedModal.has_value()
                    || session.phase() != SessionPhase::Exploration) {
                    return execution;
                }
                if (pendingWorldMapProposal.has_value()
                    && (pendingWorldMapProposal->expiresAt <= now
                        || pendingWorldMapProposal->map != map_data.field_34
                        || pendingWorldMapProposal->phaseRevision != session.phaseRevision())) {
                    pendingWorldMapProposal.reset();
                }
                if (!pendingWorldMapProposal.has_value()) {
                    pendingWorldMapProposal = PendingWorldMapProposal {
                        *actorId,
                        map_data.field_34,
                        session.phaseRevision(),
                        {},
                        now + kWorldMapProposalLifetime,
                        { player->id },
                    };
                } else {
                    if (pendingWorldMapProposal->proposerActorId == *actorId
                        || !worldMapProposalSourceReady()
                        || !pendingWorldMapProposal->readyPlayers.insert(player->id).second) {
                        return execution;
                    }
                    execution.actorId = pendingWorldMapProposal->proposerActorId;
                    if (allConnectedPlayersReady(pendingWorldMapProposal->readyPlayers)) {
                        if (session.transitionTo(SessionPhase::Transition) != LocalSessionError::None) {
                            return execution;
                        }
                        activeSharedModal = ActiveSharedModal {
                            pendingWorldMapProposal->proposerActorId,
                            SharedModalKind::WorldMap,
                        };
                        approvedWorldMapProposerActorId = pendingWorldMapProposal->proposerActorId;
                        approvedWorldMapProposalSequence = pendingWorldMapProposal->proposalSequence;
                        selectedWorldMapRoute.reset();
                        WorldMapState approvedPosition;
                        worldmap_capture_state(approvedPosition);
                        worldMapOriginX = approvedPosition.x;
                        worldMapOriginY = approvedPosition.y;
                        worldMapDeparted = false;
                        pendingWorldMapProposal.reset();
                    }
                }
            } else if (pendingWorldMapProposal.has_value()
                && session.phase() == SessionPhase::Exploration) {
                if (pendingWorldMapProposal->expiresAt <= now
                    || pendingWorldMapProposal->map != map_data.field_34
                    || pendingWorldMapProposal->phaseRevision != session.phaseRevision()) {
                    pendingWorldMapProposal.reset();
                    return execution;
                }
                execution.actorId = pendingWorldMapProposal->proposerActorId;
                pendingWorldMapProposal.reset();
            } else if (activeSharedModal.has_value()
                && activeSharedModal->kind == SharedModalKind::WorldMap
                && activeSharedModal->actorId == *actorId
                && session.phase() == SessionPhase::Transition) {
                int cityMap = -1;
                int cityEntrance = -1;
                bool enterCity = worldmap_multiplayer_position_is_city()
                    && worldmap_multiplayer_choose_destination(false, 0, true,
                        &cityMap, &cityEntrance);
                // Closing the map inside the same town keeps the current
                // positions. A submap such as a cave returns to its town.
                if (worldMapDeparted || (enterCity && cityMap != map_data.field_34)) {
                    execution.arrival = completeWorldMapTravel(
                        enterCity ? WorldMapArrivalKind::City : WorldMapArrivalKind::Terrain, 0);
                    if (!execution.arrival.has_value()) return execution;
                } else {
                    if (session.transitionTo(SessionPhase::Exploration) != LocalSessionError::None) {
                        return execution;
                    }
                    activeSharedModal.reset();
                    approvedWorldMapProposerActorId = {};
                    approvedWorldMapProposalSequence = {};
                    selectedWorldMapRoute.reset();
                    worldmap_authoritative_travel_cancel();
                    worldMapOriginX = -1;
                    worldMapOriginY = -1;
                }
            } else {
                return execution;
            }
            execution.status = CommandExecutionStatus::Applied;
            execution.phase = session.phase();
            execution.phaseRevision = session.phaseRevision();
            return execution;
        }

        if (command.open) {
            if (activeSharedModal.has_value()
                || session.phase() != SessionPhase::Exploration
                || session.transitionTo(sharedModalPhase(command.kind)) != LocalSessionError::None) {
                return execution;
            }
            activeSharedModal = ActiveSharedModal { *actorId, command.kind };
        } else {
            if (!activeSharedModal.has_value()
                || activeSharedModal->actorId != *actorId
                || activeSharedModal->kind != command.kind
                || session.phase() != sharedModalPhase(command.kind)
                || session.transitionTo(SessionPhase::Exploration) != LocalSessionError::None) {
                return execution;
            }
            activeSharedModal.reset();
            npcBarterState.reset();
            pendingScriptedNpcBarter.reset();
        }

        execution.status = CommandExecutionStatus::Applied;
        execution.phase = session.phase();
        execution.phaseRevision = session.phaseRevision();
        return execution;
    }

    CommandExecutionStatus setWorldMapRoute(Object* actor, const WorldMapRouteCommand& command) override
    {
        std::optional<EntityId> actorId = session.entities().findEntity(actor);
        if (!actorId.has_value()
            || !activeSharedModal.has_value()
            || activeSharedModal->kind != SharedModalKind::WorldMap
            || activeSharedModal->actorId != *actorId
            || session.phase() != SessionPhase::Transition
            || !isValid(command)) {
            return CommandExecutionStatus::InvalidAction;
        }
        if (command.clear) {
            selectedWorldMapRoute.reset();
        } else {
            selectedWorldMapRoute = std::make_pair(command.targetX, command.targetY);
        }
        worldmap_authoritative_travel_cancel();
        return CommandExecutionStatus::Applied;
    }

    NpcBarterExecution npcBarter(Object* actor, const NpcBarterCommand& command) override
    {
        NpcBarterExecution execution;
        Object* seller = session.entities().findObject(command.sellerId);
        auto buyerId = session.entities().findEntity(actor).value_or(EntityId {});
        if (worldMode != NetworkLaunchMode::Host || session.phase() != SessionPhase::Dialogue
            || isInCombat() || actor == nullptr || seller == nullptr || isPlayerActor(seller)
            || FID_TYPE(seller->fid) != OBJ_TYPE_CRITTER || !critter_is_active(seller)
            || !critter_is_active(actor) || actor->elevation != seller->elevation
            || !dialoguePresentation.has_value() || dialoguePresentation->targetId != command.sellerId
            || !activeSharedModal.has_value() || activeSharedModal->kind != SharedModalKind::Dialogue
            || !isValid(command)) return execution;
        if (command.action == NpcBarterAction::Begin) {
            Proto* proto = nullptr;
            if (npcBarterState.has_value() || directTradeController.active()
                || proto_ptr(seller->pid, &proto) != 0) return execution;
            if ((proto->critter.data.flags & CRITTER_BARTER) == 0) {
                ScopedPlayerFeedback feedback(actor);
                MessageListItem message;
                message.num = 903;
                if (message_search(&proto_main_msg_file, &message)) display_print(message.text);
                return execution;
            }
            if (!registerUntrackedInventory(seller)) return execution;
            npcBarterState = NpcBarterState { buyerId, command.sellerId, nextNpcBarterRevision++ };
        } else {
            if (!npcBarterState.has_value() || npcBarterState->buyerId != buyerId
                || npcBarterState->sellerId != command.sellerId || npcBarterState->revision != command.revision) return execution;
            if (command.action == NpcBarterAction::Cancel) {
                npcBarterState->status = NpcBarterStatus::Cancelled;
            } else if (command.action == NpcBarterAction::Offer) {
                if (!validateNpcBarterOffer(actor, command.buyerOffer, false)
                    || !validateNpcBarterOffer(seller, command.sellerOffer, true)) return execution;
                npcBarterState->buyerOffer = command.buyerOffer;
                npcBarterState->sellerOffer = command.sellerOffer;
                npcBarterState->status = NpcBarterStatus::Negotiating;
            } else if (command.action == NpcBarterAction::Accept) {
                auto& state = *npcBarterState;
                priceNpcBarter(actor, seller, state);
                bool valid = (!state.buyerOffer.items.empty() || state.buyerOffer.caps != 0)
                    && validateNpcBarterOffer(actor, state.buyerOffer, false)
                    && validateNpcBarterOffer(seller, state.sellerOffer, true)
                    && state.offeredValue >= state.askingValue;
                DirectTradeCommitPlan plan;
                plan.legs[0] = { {}, buyerId, {}, command.sellerId, state.buyerOffer };
                plan.legs[1] = { {}, command.sellerId, {}, buyerId, state.sellerOffer };
                state.status = valid && applyDirectTradePlan(plan, true)
                    ? NpcBarterStatus::Committed : NpcBarterStatus::Rejected;
            }
            npcBarterState->revision = nextNpcBarterRevision++;
        }
        if (npcBarterState->status == NpcBarterStatus::Negotiating) priceNpcBarter(actor, seller, *npcBarterState);
        execution.state = *npcBarterState;
        ScopedPlayerFeedback feedback(actor);
        if (npcBarterState->status == NpcBarterStatus::Committed) {
            char text[] = "Trade accepted."; display_print(text);
        } else if (npcBarterState->status == NpcBarterStatus::Rejected) {
            char text[] = "The offer was rejected."; display_print(text);
        }
        execution.status = CommandExecutionStatus::Applied;
        if (npcBarterState->status == NpcBarterStatus::Committed || npcBarterState->status == NpcBarterStatus::Cancelled) {
            dialogueVotes.deferDeadlineUntil(combatClockMilliseconds() + 60000);
            npcBarterState.reset();
            pendingScriptedNpcBarter.reset();
        }
        return execution;
    }

    DirectTradeExecution directTrade(Object* actor, PlayerId playerId,
        const DirectTradeCommand& command) override
    {
        DirectTradeExecution execution;
        if (worldMode != NetworkLaunchMode::Host || actor == nullptr
            || isInCombat()) return execution;

        DirectTradeResult result = DirectTradeResult::InvalidState;
        if (command.action == DirectTradeAction::Begin) {
            Object* other = session.entities().findObject(command.otherActorId);
            PlayerCharacterState* otherPlayer = session.players().find(
                command.otherPlayerId);
            if (directTradeController.active() || activeSharedModal.has_value()
                || session.phase() != SessionPhase::Exploration
                || other == nullptr || otherPlayer == nullptr
                || otherPlayer->actorId != command.otherActorId
                || !isAdjacentPlayerActor(actor, other)) return execution;
            result = directTradeController.begin(command.tradeId, playerId,
                session.entities().findEntity(actor).value_or(EntityId {}),
                command.otherPlayerId, command.otherActorId);
            if (result != DirectTradeResult::Accepted
                || session.transitionTo(SessionPhase::Dialogue)
                    != LocalSessionError::None) {
                directTradeController.clear();
                npcBarterState.reset();
                pendingScriptedNpcBarter.reset();
                return execution;
            }
            activeSharedModal = ActiveSharedModal {
                session.entities().findEntity(actor).value(),
                SharedModalKind::Barter,
            };
        } else {
            if (!directTradeController.active()
                || directTradeController.state().tradeId != command.tradeId
                || activeSharedModal == std::nullopt
                || activeSharedModal->kind != SharedModalKind::Barter) {
                return execution;
            }
            if (command.action == DirectTradeAction::SetOffer) {
                for (const DirectTradeItemOffer& offered
                    : command.offer.items) {
                    Object* item = session.entities().findObject(offered.itemId);
                    if (item == nullptr || item->owner != actor
                        || item->pid == PROTO_ID_MONEY
                        || (item->flags & (OBJECT_EQUIPPED | OBJECT_USED)) != 0
                        || item_count(actor, item)
                            < static_cast<int>(offered.quantity)) {
                        return execution;
                    }
                }
                if (command.offer.caps
                    > static_cast<std::uint32_t>(item_caps_total(actor))) {
                    return execution;
                }
                result = directTradeController.setOffer(playerId, command.revision,
                    command.offer);
            } else if (command.action == DirectTradeAction::Confirm) {
                result = directTradeController.confirm(playerId, command.revision);
                if (result == DirectTradeResult::ReadyToCommit) {
                    std::optional<DirectTradeCommitPlan> plan
                        = directTradeController.commitPlan();
                    if (!plan.has_value() || !applyDirectTradePlan(*plan)) {
                        if (directTradeController.invalidateCommit(command.revision)
                            != DirectTradeResult::Accepted) return execution;
                    } else {
                        if (directTradeController.commit(command.revision)
                                != DirectTradeResult::Accepted
                            || session.transitionTo(SessionPhase::Exploration)
                                != LocalSessionError::None) return execution;
                        activeSharedModal.reset();
                        execution.inventoryChanged = true;
                    }
                    result = DirectTradeResult::Accepted;
                }
            } else if (command.action == DirectTradeAction::Cancel) {
                result = directTradeController.cancel(playerId, command.revision);
                if (result == DirectTradeResult::Accepted) {
                    if (session.transitionTo(SessionPhase::Exploration)
                        != LocalSessionError::None) return execution;
                    activeSharedModal.reset();
                }
            }
        }
        if (result != DirectTradeResult::Accepted) return execution;
        execution.status = CommandExecutionStatus::Applied;
        execution.state = directTradeController.state();
        execution.phase = session.phase();
        execution.phaseRevision = session.phaseRevision();
        return execution;
    }

    InventoryTransferExecution transferInventory(Object* actor,
        Object* source,
        Object* destination,
        Object* item,
        const InventoryTransferCommand& command) override
    {
        InventoryTransferExecution execution;
        if (actor == nullptr || source == nullptr || destination == nullptr || item == nullptr) {
            return execution;
        }
        auto activeLoot = activeLootTargets.find(actor);
        Object* sourceTop = topEnvironmentOrSelf(source);
        Object* destinationTop = topEnvironmentOrSelf(destination);
        Object* otherTop = sourceTop == actor ? destinationTop : sourceTop;
        bool theftTransfer = networkWorldIsTheftTarget(actor, otherTop);
        bool lootTransfer = activeLoot != activeLootTargets.end()
            && activeLoot->second == otherTop
            && !isPlayerActor(otherTop)
            && lootTargetIsInRange(actor, otherTop);
        bool ownedTransfer = sourceTop == actor && destinationTop == actor;
        // Native item_move does not protect against moving a container into
        // itself or one of its descendants. Check before changing ownership.
        for (Object* holder = destination; holder != nullptr; holder = holder->owner) {
            if (holder == item) return execution;
        }
        bool takingLoot = lootTransfer && !theftTransfer && sourceTop == otherTop;
        bool playerGift = item != nullptr
            && source == actor
            && destination == destinationTop
            && isAdjacentPlayerActor(actor, destination)
            && (item->flags & (OBJECT_EQUIPPED | OBJECT_USED)) == 0;
        bool combat = session.phase() == SessionPhase::Combat;
        auto actorId = session.entities().findEntity(actor).value_or(EntityId {});
        auto access = openInventories.find(actorId);
        bool lootOpen = combatLootAccess(actor, command.turnRevision);
        if (combat ? (!ownedTransfer && !lootTransfer) || !combatActionAllowed(actor, command.turnRevision)
                || (lootTransfer ? !lootOpen
                    : (access == openInventories.end() || access->second != command.turnRevision) && !lootOpen)
            : session.phase() != SessionPhase::Exploration || isInCombat() || command.turnRevision != 0) {
            return execution;
        }
        if ((sourceTop != actor && destinationTop != actor)
            || (sourceTop == actor && item != nullptr
                && (item->flags & (OBJECT_EQUIPPED | OBJECT_USED)) != 0)
            || (!lootTransfer && !theftTransfer && !playerGift && !ownedTransfer)) {
            return execution;
        }
        if (theftTransfer) {
            if (item->owner != source || item_count(source, item) != static_cast<int>(command.sourceQuantity)
                || command.quantity == 0 || command.quantity > command.sourceQuantity
                || (item->flags & (OBJECT_EQUIPPED | OBJECT_USED)) != 0
                || !describeItem(item, execution.itemDescriptor)) return execution;
            TheftAccess& access = theftAccess.at(actor);
            int previousCount = gStealCount;
            int previousSize = gStealSize;
            access.attempts = std::min(access.attempts + 1, 1000000);
            gStealCount = access.attempts;
            gStealSize = item_size(item);
            ScopedPlayerFeedback feedback(actor);
            int stolen = skill_check_stealing(actor, otherTop, item, sourceTop == actor);
            gStealCount = previousCount;
            gStealSize = previousSize;
            if (stolen != 1) {
                if (stolen == 0) finishTheft(actor, true);
                return execution;
            }
            if (!applyInventoryTransfer(source, destination, item, command.quantity, false)) return execution;
            access.experience = std::min(300, access.experience + access.nextExperience);
            access.nextExperience = std::min(300, access.nextExperience + 10);
            execution.itemId = command.itemId;
            execution.remainderItemId = lastSplitEntityId;
            execution.status = CommandExecutionStatus::Applied;
            return execution;
        }

        LootDistributionState priorDistribution = lootDistribution.state();
        if (takingLoot && item != nullptr && item->pid == PROTO_ID_MONEY) {
            if (item_count(source, item) != static_cast<int>(command.sourceQuantity)
                || command.quantity > command.sourceQuantity) return execution;
            std::optional<std::vector<PlayerCapShare>> shares
                = lootDistribution.splitCaps(command.quantity);
            if (!shares.has_value()) return execution;
            for (PlayerCapShare& share : *shares) {
                share.actorId = session.playerActorId(share.playerId);
                if (!isValid(share.actorId)
                    || session.entities().findObject(share.actorId) == nullptr) {
                    lootDistribution.restore(priorDistribution);
                    return execution;
                }
            }
            if (item_caps_adjust(source, -static_cast<int>(command.quantity))
                != 0) {
                lootDistribution.restore(priorDistribution);
                return execution;
            }
            std::size_t adjusted = 0;
            for (; adjusted < shares->size(); adjusted++) {
                Object* recipient = session.entities().findObject(
                    (*shares)[adjusted].actorId);
                if ((*shares)[adjusted].caps != 0
                    && item_caps_adjust(recipient,
                        static_cast<int>((*shares)[adjusted].caps)) != 0) {
                    break;
                }
            }
            if (adjusted != shares->size()) {
                while (adjusted > 0) {
                    adjusted--;
                    if ((*shares)[adjusted].caps != 0) {
                        item_caps_adjust(session.entities().findObject(
                                (*shares)[adjusted].actorId),
                            -static_cast<int>((*shares)[adjusted].caps));
                    }
                }
                item_caps_adjust(source, static_cast<int>(command.quantity));
                lootDistribution.restore(priorDistribution);
                return execution;
            }
            auto rollbackDistribution = [&]() {
                for (const PlayerCapShare& share : *shares) {
                    if (share.caps != 0) {
                        item_caps_adjust(session.entities().findObject(
                                share.actorId),
                            -static_cast<int>(share.caps));
                    }
                }
                item_caps_adjust(source, static_cast<int>(command.quantity));
                lootDistribution.restore(priorDistribution);
            };
            if (!registerUntrackedInventory(source)) {
                rollbackDistribution();
                return execution;
            }
            for (const PlayerCapShare& share : *shares) {
                if (!registerUntrackedInventory(
                        session.entities().findObject(share.actorId))) {
                    rollbackDistribution();
                    return execution;
                }
            }
            execution.capShares = std::move(*shares);
            execution.status = CommandExecutionStatus::Applied;
            return execution;
        }

        if (takingLoot && item != nullptr
            && FID_TYPE(otherTop->fid) == OBJ_TYPE_CRITTER
            && critter_is_dead(otherTop)) {
            std::vector<PlayerId> eligible = lootDistribution.state().roster;
            std::optional<PlayerId> priority
                = lootDistribution.takeLootPriority(eligible);
            Object* priorityActor = priority.has_value()
                ? session.entities().findObject(
                    session.playerActorId(*priority))
                : nullptr;
            if (priorityActor == nullptr) {
                lootDistribution.restore(priorDistribution);
                return execution;
            }
            destination = priorityActor;
            execution.destinationId = session.playerActorId(*priority);
        }

        execution.itemId = command.itemId;

        if (item_count(source, item) != static_cast<int>(command.sourceQuantity)
            || !describeItem(item, execution.itemDescriptor)
            || !applyInventoryTransfer(source, destination, item, command.quantity, false)) {
            lootDistribution.restore(priorDistribution);
            return execution;
        }
        execution.remainderItemId = lastSplitEntityId;
        execution.status = CommandExecutionStatus::Applied;
        return execution;
    }

    ItemDropExecution dropItem(Object* actor,
        Object* source,
        Object* item,
        const ItemDropCommand& command) override
    {
        ItemDropExecution execution;
        if (actor == nullptr
            || source == nullptr
            || item == nullptr
            || (item->flags & (OBJECT_EQUIPPED | OBJECT_USED)) != 0
            || isInCombat()
            || topEnvironmentOrSelf(source) != actor
            || !hexGridTileIsValid(actor->tile)
            || !elevationIsValid(actor->elevation)) {
            return execution;
        }

        execution.itemId = command.itemId;

        if (item_count(source, item) != static_cast<int>(command.sourceQuantity)
            || (command.quantity > 1 && item->pid != PROTO_ID_MONEY)
            || !applyItemDrop(source, item, command.quantity)
            || !describeItem(item, execution.itemDescriptor)) {
            return execution;
        }

        execution.remainderItemId = lastSplitEntityId;
        execution.tile = item->tile;
        execution.elevation = item->elevation;
        execution.status = CommandExecutionStatus::Applied;
        return execution;
    }
};

NetworkCommandExecutor commandExecutor;

void finishTheft(Object* actor, bool caught)
{
    auto found = theftAccess.find(actor);
    if (found == theftAccess.end()) return;
    TheftAccess access = found->second;
    theftAccess.erase(found);
    activeLootTargets.erase(actor);
    PlayerCharacterState* player = playerStateForActor(actor);
    if (player == nullptr) return;
    deferredEvents.push_back(GameEvent { {}, CommandSequence { UINT64_MAX },
        SharedModalStateChangedEvent { player->actorId, SharedModalKind::Theft, false,
            session.phase(), session.phaseRevision() } });
    ScopedActingPlayerContext context(*player, actor);
    ScopedPlayerFeedback feedback(actor);
    if (!session.entities().findEntity(access.target).has_value()) return;
    if (!caught) {
        inven_steal_award_xp(actor, access.target, access.experience);
    } else if (access.attempts > 0) {
        int sid = -1;
        if (obj_sid(access.target, &sid) != -1) {
            scr_set_objs(sid, actor, nullptr);
            exec_script_proc(sid, SCRIPT_PROC_PICKUP);
        }
    }
}

bool registerWorldObjects()
{
    combatTurns.stop();
    openInventories.clear();
    openLootTurns.clear();
    worldDoors.clear();
    worldScenery.clear();
    worldExitGrids.clear();
    worldItems.clear();
    worldCritters.clear();
    reservedPickupTargets.clear();
    pendingPickups.clear();
    deferredEvents.clear();
    dialogueVotes.clear();
    dialoguePresentation.reset();
    pendingTalk.reset();
    dialogueCause = {};
    nextDialogueRevision = 1;
    activeLootTargets.clear();
    theftAccess.clear();
    activeSharedModal.reset();
    approvedWorldMapProposerActorId = {};
    approvedWorldMapProposalSequence = {};
    selectedWorldMapRoute.reset();
    pendingWorldMapProposal.reset();
    std::vector<Object*> doors;
    std::vector<Object*> scenery;
    std::vector<Object*> exitGrids;
    std::vector<Object*> items;
    std::vector<Object*> critters;
    // The native iterator revisits its first occupied tile. Treat the scan
    // as a set so a placed object receives only one snapshot section entry.
    std::unordered_set<Object*> seenObjects;
    for (Object* object = obj_find_first(); object != nullptr; object = obj_find_next()) {
        if (!seenObjects.insert(object).second) continue;
        if (object == obj_dude || object == peerActor) {
            continue;
        }
        int objectType = FID_TYPE(object->fid);
        if (isExitGrid(object)) {
            exitGrids.push_back(object);
        } else if (objectType == OBJ_TYPE_SCENERY && obj_is_a_portal(object)) {
            doors.push_back(object);
        } else if (objectType == OBJ_TYPE_SCENERY
            && object->tile >= 0) {
            scenery.push_back(object);
        } else if (objectType == OBJ_TYPE_ITEM
            && object->owner == nullptr
            && object->tile >= 0) {
            items.push_back(object);
        } else if (objectType == OBJ_TYPE_CRITTER) {
            critters.push_back(object);
        }
    }

    auto stableObjectOrder = [](const Object* lhs, const Object* rhs) {
        return std::tie(lhs->elevation, lhs->tile, lhs->pid, lhs->id, lhs->fid)
            < std::tie(rhs->elevation, rhs->tile, rhs->pid, rhs->id, rhs->fid);
    };
    std::sort(doors.begin(), doors.end(), stableObjectOrder);
    std::sort(scenery.begin(), scenery.end(), stableObjectOrder);
    std::sort(exitGrids.begin(), exitGrids.end(), stableObjectOrder);
    std::sort(items.begin(), items.end(), stableObjectOrder);
    std::sort(critters.begin(), critters.end(), [](const Object* lhs, const Object* rhs) {
        return std::tie(lhs->id, lhs->pid, lhs->elevation)
            < std::tie(rhs->id, rhs->pid, rhs->elevation);
    });

    auto registerInventory = [&](auto&& self, Object* owner) -> bool {
        std::vector<Object*> inventoryItems;
        Inventory* inventory = &owner->data.inventory;
        inventoryItems.reserve(inventory->length);
        for (int index = 0; index < inventory->length; index++) {
            inventoryItems.push_back(inventory->items[index].item);
        }
        std::sort(inventoryItems.begin(), inventoryItems.end(), stableObjectOrder);
        for (Object* item : inventoryItems) {
            EntityRegistrationResult registration = session.registerWorldObject(item);
            if (!registration) {
                return false;
            }
            worldItems.emplace_back(registration.entityId, item);
            if (!self(self, item)) {
                return false;
            }
        }
        return true;
    };

    Object* host = session.entities().findObject(session.playerActorId(kHostPlayerId));
    Object* guest = session.entities().findObject(session.playerActorId(kGuestPlayerId));
    if (host == nullptr || guest == nullptr) {
        return false;
    }

    // Exit grids are absent from saved snapshots. Give the sorted map exits
    // fixed IDs after the two players instead of depending on the allocator's
    // history, which differs when a guest loads a host's saved map.
    std::uint32_t exitId = 3;
    for (Object* exitGrid : exitGrids) {
        EntityId entityId { exitId++ };
        if (session.entities().findObject(entityId) != exitGrid
            && session.entities().restoreObject(entityId, exitGrid)
                != EntityRegistryError::None) {
            return false;
        }
        worldExitGrids.emplace_back(entityId, exitGrid);
    }

    for (Object* door : doors) {
        EntityRegistrationResult registration = session.registerWorldObject(door);
        if (!registration) {
            return false;
        }
        worldDoors.emplace_back(registration.entityId, door);
    }
    for (Object* object : scenery) {
        EntityRegistrationResult registration = session.registerWorldObject(object);
        if (!registration) {
            return false;
        }
        worldScenery.emplace_back(registration.entityId, object);
    }
    for (Object* critter : critters) {
        EntityRegistrationResult registration = session.registerWorldObject(critter);
        if (!registration) {
            return false;
        }
        worldCritters.emplace_back(registration.entityId, critter);
    }
    // Register fixed map entities before inventories. Map-enter scripts run
    // only on the host and can create/remove items, but must not shift the IDs
    // of matching exits, doors, scenery, and installed critters on replicas.
    if (!registerInventory(registerInventory, host)
        || !registerInventory(registerInventory, guest)) {
        return false;
    }
    for (Object* item : items) {
        EntityRegistrationResult registration = session.registerWorldObject(item);
        if (!registration) {
            return false;
        }
        worldItems.emplace_back(registration.entityId, item);
        if (!registerInventory(registerInventory, item)) {
            return false;
        }
    }
    for (Object* critter : critters) {
        if (!registerInventory(registerInventory, critter)) {
            return false;
        }
    }
    for (Object* door : doors) {
        if (!registerInventory(registerInventory, door)) return false;
    }
    for (Object* object : scenery) {
        if (!registerInventory(registerInventory, object)) return false;
    }
    return true;
}

void erasePeerActor()
{
    if (peerActor == nullptr) {
        return;
    }

    register_clear(peerActor);
    peerActor->flags &= ~OBJECT_NO_REMOVE;
    obj_erase_object(peerActor, nullptr);
    peerActor = nullptr;
}

Object* createPeerActor()
{
    if (obj_dude == nullptr || obj_dude->tile == -1) {
        return nullptr;
    }

    Object* actor = nullptr;
    if (obj_pid_new(&actor, obj_dude->pid) == -1) {
        return nullptr;
    }

    actor->flags |= OBJECT_NO_SAVE;
    actor->flags &= ~OBJECT_NO_REMOVE;
    actor->data.critter.combat.aiPacket = 0;
    actor->data.critter.combat.team = obj_dude->data.critter.combat.team;
    if (obj_change_fid(actor, obj_dude->fid, nullptr) == -1
        || obj_attempt_placement(actor, obj_dude->tile, obj_dude->elevation, 2) == -1) {
        obj_erase_object(actor, nullptr);
        return nullptr;
    }
    return actor;
}

bool refreshPlayer(PlayerId playerId, bool healToFull = true)
{
    PlayerCharacterState* player = session.players().find(playerId);
    Object* actor = player != nullptr ? session.entities().findObject(player->actorId) : nullptr;
    if (player == nullptr || actor == nullptr) {
        return false;
    }

    ScopedActingPlayerContext actingPlayer(*player, actor);
    stat_recalc_derived(actor);
    if (healToFull) {
        int hitPoints = critter_get_hits(actor);
        int maximumHitPoints = stat_level(actor, STAT_MAXIMUM_HIT_POINTS);
        critter_adjust_hits(actor, maximumHitPoints - hitPoints);
    }
    if (updatePlayerGenderAppearance(actor) == -1) {
        return false;
    }
    dude_stand(actor, actor->rotation, -1);
    return true;
}

} // namespace

bool networkWorldEnter(NetworkLaunchMode mode,
    const CharacterCreationSheet& localSheet,
    const CharacterCreationSheet& peerSheet, bool seedStartingKit)
{
    networkWorldLeave();
    PlayerId expectedLocalPlayer = mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    PlayerId expectedPeerPlayer = mode == NetworkLaunchMode::Host ? kGuestPlayerId : kHostPlayerId;
    if ((mode != NetworkLaunchMode::Host && mode != NetworkLaunchMode::Join)
        || localSheet.playerId != expectedLocalPlayer
        || peerSheet.playerId != expectedPeerPlayer) {
        return false;
    }
    worldMode = mode;

    peerActor = createPeerActor();
    if (peerActor == nullptr) {
        return false;
    }
    if (mode == NetworkLaunchMode::Host && seedStartingKit) {
        auto copyStartingInventory = [&](auto&& self, Object* source, Object* destination) -> bool {
            for (int index = 0; index < source->data.inventory.length; ++index) {
                const InventoryItem& entry = source->data.inventory.items[index];
                ItemDescriptor descriptor;
                if (!describeItem(entry.item, descriptor)) return false;
                Object* copy = createItem(descriptor);
                if (copy == nullptr) return false;
                if (obj_disconnect(copy, nullptr) == -1
                    || item_add_force(destination, copy, entry.quantity) != 0) {
                    obj_erase_object(copy, nullptr);
                    return false;
                }
                if (!self(self, entry.item, copy)) return false;
            }
            return true;
        };
        if (!copyStartingInventory(copyStartingInventory, obj_dude, peerActor)) {
            erasePeerActor();
            return false;
        }
    }

    int hostTile = obj_dude->tile;
    int guestTile = peerActor->tile;
    Object* hostActor = mode == NetworkLaunchMode::Host ? obj_dude : peerActor;
    Object* guestActor = mode == NetworkLaunchMode::Host ? peerActor : obj_dude;
    if (mode == NetworkLaunchMode::Join
        && (obj_move_to_tile(obj_dude, guestTile, obj_dude->elevation, nullptr) == -1
            || obj_move_to_tile(peerActor, hostTile, peerActor->elevation, nullptr) == -1)) {
        erasePeerActor();
        return false;
    }

    const CharacterCreationSheet& hostSheet = localSheet.playerId == kHostPlayerId ? localSheet : peerSheet;
    const CharacterCreationSheet& guestSheet = localSheet.playerId == kGuestPlayerId ? localSheet : peerSheet;
    if (hostSheet.playerId != kHostPlayerId
        || guestSheet.playerId != kGuestPlayerId
        || session.start(hostActor, guestActor) != LocalSessionError::None
        || session.submitCharacterSheet(hostSheet) != CharacterLobbyError::None
        || session.submitCharacterSheet(guestSheet) != CharacterLobbyError::None) {
        session.stop();
        erasePeerActor();
        return false;
    }

    PlayerId localPlayerId = mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    PlayerId peerPlayerId = mode == NetworkLaunchMode::Host ? kGuestPlayerId : kHostPlayerId;
    PlayerCharacterState* localPlayer = session.players().find(localPlayerId);
    PlayerCharacterState* remotePlayer = session.players().find(peerPlayerId);
    if (localPlayer == nullptr
        || remotePlayer == nullptr
        || bindLocalPlayer(session, localPlayerId) != LocalPlayerError::None
        || session.transitionTo(SessionPhase::Loading) != LocalSessionError::None
        || session.transitionTo(SessionPhase::Exploration) != LocalSessionError::None) {
        session.stop();
        erasePeerActor();
        return false;
    }

    localPlayer->ownership = PlayerOwnership::LocalControl;
    localPlayer->connection = ConnectionState::Connected;
    remotePlayer->ownership = PlayerOwnership::RemoteControl;
    remotePlayer->connection = ConnectionState::Connected;
    if (!registerWorldObjects()
        || lootDistribution.begin(session.players().playerIds())
            != LootDistributionError::None
        // A loaded host already has native HP, poison and combat state. Its
        // saved build is applied by RestoreMultiplayerSave below; refreshing
        // against the temporary lobby sheet would heal or clip those values.
        || ((seedStartingKit || mode != NetworkLaunchMode::Host) && !refreshPlayer(kHostPlayerId))
        || !refreshPlayer(kGuestPlayerId)) {
        session.stop();
        erasePeerActor();
        return false;
    }

    intface_redraw();
    commandProcessor.reset();
    WorldMapState initialMap;
    worldmap_capture_state(initialMap);
    observedFirstVisits = static_cast<std::uint32_t>(initialMap.firstVisits);
    return true;
}

bool networkWorldRestoreMultiplayerSave(const MultiplayerSaveSidecar& sidecar,
    Object* savedGuestActor)
{
    Object* guest = networkWorldPlayerActor(kGuestPlayerId);
    if (worldMode != NetworkLaunchMode::Host
        || guest == nullptr
        || savedGuestActor == nullptr
        || PID_TYPE(savedGuestActor->pid) != OBJ_TYPE_CRITTER
        || savedGuestActor->pid != guest->pid
        || !hexGridTileIsValid(savedGuestActor->tile)
        || !elevationIsValid(savedGuestActor->elevation)
        || savedGuestActor->rotation < 0
        || savedGuestActor->rotation >= ROTATION_COUNT) {
        std::fprintf(stderr,
            "Multiplayer recovery actor preflight failed: mode=%d guest=%d saved=%d saved_type=%d pid=%d/%d tile=%d elevation=%d rotation=%d.\n",
            static_cast<int>(worldMode), guest != nullptr ? 1 : 0,
            savedGuestActor != nullptr ? 1 : 0,
            savedGuestActor != nullptr ? PID_TYPE(savedGuestActor->pid) : -1,
            savedGuestActor != nullptr ? savedGuestActor->pid : -1,
            guest != nullptr ? guest->pid : -1,
            savedGuestActor != nullptr ? savedGuestActor->tile : -1,
            savedGuestActor != nullptr ? savedGuestActor->elevation : -1,
            savedGuestActor != nullptr ? savedGuestActor->rotation : -1);
        return false;
    }
    MultiplayerSaveError saveError = validateMultiplayerSave(sidecar);
    LocalSessionError playerError = saveError == MultiplayerSaveError::None
        ? session.restorePlayerCharacters(sidecar)
        : LocalSessionError::InvalidSaveState;
    LootDistributionError lootError = playerError == LocalSessionError::None
        ? lootDistribution.restore(sidecar.lootDistribution)
        : LootDistributionError::InvalidState;
    if (saveError != MultiplayerSaveError::None
        || playerError != LocalSessionError::None
        || lootError != LootDistributionError::None) {
        std::fprintf(stderr,
            "Multiplayer recovery metadata preflight failed: save=%d players=%d loot=%d.\n",
            static_cast<int>(saveError), static_cast<int>(playerError),
            static_cast<int>(lootError));
        return false;
    }

    register_clear(guest);
    obj_inven_free(&guest->data.inventory);
    guest->data.critter = savedGuestActor->data.critter;
    guest->data.critter.combat.whoHitMe = nullptr;
    attachInventory(guest, savedGuestActor->data.inventory);
    savedGuestActor->data.inventory = {};
    queue_bind_loaded_owner(savedGuestActor->id, guest);

    int savedFid = savedGuestActor->fid;
    int savedTile = savedGuestActor->tile;
    int savedElevation = savedGuestActor->elevation;
    int savedRotation = savedGuestActor->rotation;
    if (obj_change_fid(guest, savedFid, nullptr) == -1
        || obj_attempt_placement(guest, savedTile, savedElevation, 2) == -1) {
        std::fprintf(stderr,
            "Multiplayer recovery guest placement failed: fid=%d tile=%d elevation=%d.\n",
            savedFid, savedTile, savedElevation);
        return false;
    }
    dude_stand(guest, savedRotation, -1);

    session.clearWorldEntities();
    if (!registerWorldObjects()) {
        std::fprintf(stderr,
            "Multiplayer recovery world registration failed.\n");
        return false;
    }
    Object* host = networkWorldPlayerActor(kHostPlayerId);

    // Native map loading can legitimately renumber inventory objects when
    // map scripts create or remove other entities. The save-sidecar ownership
    // contract is therefore restored by player and inventory topology; the
    // new session's canonical IDs are published in its first snapshot.
    for (const auto& participant : std::array<std::pair<PlayerId, Object*>, 2> {
             std::make_pair(kHostPlayerId, host),
             std::make_pair(kGuestPlayerId, guest) }) {
        bool actorOwnershipPresent = false;
        std::size_t savedItemCount = 0;
        for (const SavedEntityOwnership& ownership : sidecar.ownership) {
            if (ownership.ownerId != participant.first) continue;
            if (ownership.entityId == session.playerActorId(participant.first)) {
                actorOwnershipPresent = true;
            } else {
                savedItemCount++;
            }
        }
        std::size_t loadedItemCount = 0;
        for (const auto& entry : worldItems) {
            if (topEnvironmentOrSelf(entry.second) == participant.second) {
                loadedItemCount++;
            }
        }
        Object* actor = session.entities().findObject(
            session.playerActorId(participant.first));
        if (!actorOwnershipPresent
            || actor != participant.second
            || savedItemCount != loadedItemCount) {
            std::fprintf(stderr,
                "Multiplayer recovery ownership topology mismatch: owner=%u actor=%d saved_items=%zu loaded_items=%zu.\n",
                participant.first.value,
                actorOwnershipPresent && actor == participant.second ? 1 : 0,
                savedItemCount, loadedItemCount);
            return false;
        }
    }
    sharedActivity.assign(sidecar.sharedActivity.begin(),
        sidecar.sharedActivity.end());
    nextSharedActivityId = sharedActivity.empty()
        ? 1 : sharedActivity.back().id + 1;
    if (!refreshPlayer(kHostPlayerId, false)
        || !refreshPlayer(kGuestPlayerId, false)) {
        std::fprintf(stderr,
            "Multiplayer recovery player refresh failed.\n");
        return false;
    }
    intface_redraw();
    return true;
}

bool networkWorldBeginEnding()
{
    if (worldMode != NetworkLaunchMode::Host || !session.isActive()) {
        return false;
    }
    combatTurns.stop();
    openInventories.clear();
    openLootTurns.clear();
    directTradeController.clear();
    npcBarterState.reset();
    pendingScriptedNpcBarter.reset();
    dialogueVotes.clear();
    dialoguePresentation.reset();
    activeSharedModal.reset();
    return session.transitionTo(SessionPhase::Ending)
        == LocalSessionError::None;
}

bool networkWorldApplyPeerMove(const ActorMovementStartedEvent& movement)
{
    if (!session.isActive()) {
        return false;
    }

    PlayerCharacterState* player = session.players().findByActor(movement.actorId);
    Object* actor = player != nullptr ? session.entities().findObject(player->actorId) : nullptr;
    if (player == nullptr
        || actor == nullptr
        || !hexGridTileIsValid(movement.destinationTile)
        || movement.elevation != actor->elevation) {
        return false;
    }

    if (!movement.path.empty()) {
        if (movement.path.size() > kMaximumMovementPathLength
            || movement.path.size() > static_cast<std::size_t>(kAnimationMaximumPathLength)
            || !hexGridTileIsValid(movement.startingTile)) {
            return false;
        }

        int pathTile = movement.startingTile;
        for (std::uint8_t rotation : movement.path) {
            if (rotation >= ROTATION_COUNT) {
                return false;
            }
            pathTile = tile_num_in_direction(pathTile, rotation, 1);
            if (!hexGridTileIsValid(pathTile)) {
                return false;
            }
        }
        if (pathTile != movement.destinationTile) {
            return false;
        }
    }

    register_clear(actor);
    if (!movement.path.empty() && actor->tile != movement.startingTile) {
        Rect dirtyRect;
        if (obj_move_to_tile(actor, movement.startingTile, movement.elevation, &dirtyRect) == -1) {
            return false;
        }
        tile_refresh_rect(&dirtyRect, actor->elevation);
    }
    int requestOptions = actor == obj_dude
        ? ANIMATION_REQUEST_RESERVED
        : ANIMATION_REQUEST_UNRESERVED;
    if (register_begin(requestOptions) == -1) {
        return false;
    }
    int rc;
    if (!movement.path.empty()) {
        rc = register_object_move_along_path(actor,
            movement.destinationTile,
            movement.elevation,
            movement.path.data(),
            static_cast<int>(movement.path.size()),
            movement.running,
            0);
    } else {
        rc = movement.running
            ? register_object_run_to_tile(actor, movement.destinationTile, movement.elevation, -1, 0)
            : register_object_move_to_tile(actor, movement.destinationTile, movement.elevation, -1, 0);
    }
    int endRc = register_end();
    return rc != -1 && endRc != -1;
}

bool networkWorldApplyPeerFacing(const ActorFacingChangedEvent& facing)
{
    if (!session.isActive()) {
        return false;
    }

    PlayerCharacterState* player = session.players().findByActor(facing.actorId);
    Object* actor = player != nullptr ? session.entities().findObject(player->actorId) : nullptr;
    if (player == nullptr
        || actor == nullptr
        || facing.rotation < 0
        || facing.rotation >= ROTATION_COUNT) {
        return false;
    }

    Rect dirtyRect;
    if (obj_set_rotation(actor, facing.rotation, &dirtyRect) == -1) {
        return false;
    }
    tile_refresh_rect(&dirtyRect, actor->elevation);
    return true;
}

bool networkWorldApplyPeerDoorUse(const DoorUseStartedEvent& doorUse)
{
    if (!session.isActive()) {
        return false;
    }

    PlayerCharacterState* player = session.players().findByActor(doorUse.actorId);
    Object* actor = player != nullptr ? session.entities().findObject(player->actorId) : nullptr;
    Object* target = session.entities().findObject(doorUse.targetId);
    if (player == nullptr
        || actor == nullptr
        || target == nullptr
        || !obj_is_a_portal(target)
        || actor->elevation != target->elevation) {
        return false;
    }

    if (doorUse.open != (doorUse.frame != 0)) {
        return false;
    }

    Rect dirtyRect;
    if (obj_set_frame(target, doorUse.frame, &dirtyRect) == -1
        || (doorUse.locked ? obj_lock(target) : obj_unlock(target)) == -1) {
        return false;
    }
    tile_refresh_rect(&dirtyRect, target->elevation);
    return true;
}

bool networkWorldApplyPeerPickup(const ItemPickupStartedEvent& pickup)
{
    if (!session.isActive()) {
        return false;
    }

    PlayerCharacterState* player = session.players().findByActor(pickup.actorId);
    Object* actor = player != nullptr ? session.entities().findObject(player->actorId) : nullptr;
    Object* target = session.entities().findObject(pickup.targetId);
    if (player == nullptr || actor == nullptr || target == nullptr) {
        return false;
    }

    // The event is a presentation cue. Calling the pickup action here would
    // rerun scripts and inventory rules on the guest. The authoritative state
    // snapshot applies the resulting item ownership after the host completes
    // the action.
    return target->owner == nullptr;
}

bool networkWorldApplyPeerPickupCompletion(const ItemPickupCompletedEvent& pickup)
{
    if (!session.isActive()) {
        return false;
    }
    PlayerCharacterState* player = session.players().findByActor(pickup.actorId);
    Object* actor = player != nullptr ? session.entities().findObject(player->actorId) : nullptr;
    Object* target = session.entities().findObject(pickup.targetId);
    if (player == nullptr
        || actor == nullptr
        || target == nullptr
        || FID_TYPE(target->fid) != OBJ_TYPE_ITEM) {
        return false;
    }
    if (!pickup.succeeded) {
        return target->owner == nullptr;
    }

    ItemDescriptor actualDescriptor;
    if (target->owner == actor) {
        return item_count(actor, target) == static_cast<int>(pickup.quantity)
            && describeItem(target, actualDescriptor)
            && itemDescriptorsEqual(actualDescriptor, pickup.itemDescriptor);
    }
    if (target->owner != nullptr
        || !applyItemDescriptor(target, pickup.itemDescriptor)) {
        return false;
    }

    inventoryTransferInProgress = true;
    int rc = item_add_force(actor, target, 1);
    if (rc == 0) {
        rc = obj_disconnect(target, nullptr);
    }
    inventoryTransferInProgress = false;
    if (rc != 0 || !setInventoryQuantity(actor, target, pickup.quantity)) {
        return false;
    }
    inven_refresh_inventory_window();
    intface_redraw();
    return true;
}

bool networkWorldApplyPeerLoot(const LootStartedEvent& loot)
{
    if (!session.isActive()) {
        return false;
    }
    PlayerCharacterState* player = session.players().findByActor(loot.actorId);
    Object* actor = player != nullptr ? session.entities().findObject(player->actorId) : nullptr;
    Object* target = session.entities().findObject(loot.targetId);
    if (player == nullptr
        || actor == nullptr
        || target == nullptr
        || isPlayerActor(target)
        || !lootTargetIsInRange(actor, target)
        || (loot.turnRevision != 0
            ? session.phase() != SessionPhase::Combat || combatTurns.revision() != loot.turnRevision
                || !networkWorldCombatTurnMatches(actor)
            : session.phase() != SessionPhase::Exploration)) {
        return false;
    }
    if (actor != obj_dude) {
        return true;
    }
    activeLootTargets[actor] = target;
    if (loot.turnRevision != 0) openLootTurns[player->actorId] = loot.turnRevision;
    if (inven_loot_window_is_active()) {
        return true;
    }
    ScopedActingPlayerContext actingPlayer(*player, actor);
    return scripts_request_loot_container(actor, target) == 0;
}

bool networkWorldApplyPeerSharedModal(const SharedModalStateChangedEvent& modal)
{
    if (!session.isActive()
        || !isValid(modal.kind)
        || session.entities().findObject(modal.actorId) == nullptr
        || modal.phaseRevision == 0) {
        return false;
    }
    if (modal.kind == SharedModalKind::Theft) {
        if (modal.open || modal.phase != SessionPhase::Exploration
            || modal.phaseRevision != session.phaseRevision()) return false;
        Object* actor = session.entities().findObject(modal.actorId);
        theftAccess.erase(actor);
        activeLootTargets.erase(actor);
        return true;
    }
    if (modal.kind == SharedModalKind::WorldMap) {
        if (modal.open && modal.phase == SessionPhase::Exploration) {
            if (pendingWorldMapProposal.has_value()
                && pendingWorldMapProposal->proposerActorId == modal.actorId
                && pendingWorldMapProposal->phaseRevision == modal.phaseRevision
                && session.phase() == SessionPhase::Exploration) {
                return true;
            }
            if (session.phase() != SessionPhase::Exploration
                || modal.phaseRevision != session.phaseRevision()
                || pendingWorldMapProposal.has_value()) {
                return false;
            }
            pendingWorldMapProposal = PendingWorldMapProposal {
                modal.actorId,
                map_data.field_34,
                modal.phaseRevision,
                {},
                std::chrono::steady_clock::now() + kWorldMapProposalLifetime,
                {},
            };
            return true;
        }
        if (modal.open && modal.phase == SessionPhase::Transition) {
            if (activeSharedModal.has_value()
                && activeSharedModal->kind == modal.kind
                && activeSharedModal->actorId == modal.actorId
                && session.phase() == modal.phase
                && session.phaseRevision() == modal.phaseRevision) {
                return true;
            }
            if (activeSharedModal.has_value()
                && activeSharedModal->kind == SharedModalKind::WorldMap
                && activeSharedModal->actorId == session.playerActorId(kGuestPlayerId)
                && approvedWorldMapProposerActorId == session.playerActorId(kGuestPlayerId)
                && modal.actorId == session.playerActorId(kHostPlayerId)
                && session.phase() == SessionPhase::Transition
                && session.phaseRevision() == modal.phaseRevision) {
                activeSharedModal->actorId = modal.actorId;
                return true;
            }
            if (session.phase() != SessionPhase::Exploration
                || !pendingWorldMapProposal.has_value()
                || pendingWorldMapProposal->proposerActorId != modal.actorId
                || session.applyAuthoritativePhase(modal.phase, modal.phaseRevision) != LocalSessionError::None) {
                return false;
            }
            pendingWorldMapProposal.reset();
            activeSharedModal = ActiveSharedModal { modal.actorId, modal.kind };
            approvedWorldMapProposerActorId = modal.actorId;
            selectedWorldMapRoute.reset();
            worldmap_authoritative_travel_cancel();
            return true;
        }
        if (!modal.open && modal.phase == SessionPhase::Exploration) {
            if (pendingWorldMapProposal.has_value()) {
                if (pendingWorldMapProposal->proposerActorId != modal.actorId
                    || modal.phaseRevision != session.phaseRevision()) {
                    return false;
                }
                pendingWorldMapProposal.reset();
                return true;
            }
            if (activeSharedModal.has_value()
                && activeSharedModal->actorId == modal.actorId
                && activeSharedModal->kind == modal.kind
                && session.applyAuthoritativePhase(modal.phase, modal.phaseRevision) == LocalSessionError::None) {
                activeSharedModal.reset();
                approvedWorldMapProposerActorId = {};
                approvedWorldMapProposalSequence = {};
                selectedWorldMapRoute.reset();
                worldmap_authoritative_travel_cancel();
                return true;
            }
            return !activeSharedModal.has_value()
                && session.phase() == SessionPhase::Exploration
                && session.phaseRevision() == modal.phaseRevision;
        }
        return false;
    }
    SessionPhase expectedPhase = modal.open
        ? sharedModalPhase(modal.kind)
        : SessionPhase::Exploration;
    if (modal.phase != expectedPhase) {
        return false;
    }
    if (modal.open) {
        if (activeSharedModal.has_value()) {
            return activeSharedModal->actorId == modal.actorId
                && activeSharedModal->kind == modal.kind
                && session.phase() == modal.phase
                && session.phaseRevision() == modal.phaseRevision;
        }
        if (session.phase() != SessionPhase::Exploration) {
            return false;
        }
        activeSharedModal = ActiveSharedModal { modal.actorId, modal.kind };
    } else {
        if (activeSharedModal.has_value()
            && (activeSharedModal->actorId != modal.actorId || activeSharedModal->kind != modal.kind)) {
            return false;
        }
    }
    if (session.applyAuthoritativePhase(modal.phase, modal.phaseRevision) != LocalSessionError::None) {
        if (modal.open) {
            activeSharedModal.reset();
        }
        return false;
    }
    if (!modal.open) {
        activeSharedModal.reset();
        if (modal.kind == SharedModalKind::Dialogue) {
            dialoguePresentation.reset();
            dialogueVotes.clear();
        }
    }
    return true;
}

bool networkWorldApplyPeerWorldMapRoute(const WorldMapRouteSelectedEvent& route)
{
    if (!session.isActive()
        || session.phase() != SessionPhase::Transition
        || !activeSharedModal.has_value()
        || activeSharedModal->kind != SharedModalKind::WorldMap
        || activeSharedModal->actorId != route.actorId
        || !isValid(WorldMapRouteCommand { route.targetX, route.targetY, route.clear })) {
        return false;
    }
    if (route.clear) {
        selectedWorldMapRoute.reset();
    } else {
        selectedWorldMapRoute = std::make_pair(route.targetX, route.targetY);
    }
    worldmap_authoritative_travel_cancel();
    return true;
}

bool networkWorldApplyPeerAttack(const AttackStartedEvent& attack)
{
    if (!session.isActive()) {
        return false;
    }
    PlayerCharacterState* player = session.players().findByActor(attack.actorId);
    Object* actor = player != nullptr ? session.entities().findObject(player->actorId) : nullptr;
    Object* target = session.entities().findObject(attack.targetId);
    if (player == nullptr
        || actor == nullptr
        || target == nullptr
        || actor == target
        || FID_TYPE(target->fid) != OBJ_TYPE_CRITTER
        || attack.hitMode < 0 || attack.hitMode >= HIT_MODE_COUNT
        || attack.hitLocation < 0 || attack.hitLocation >= HIT_LOCATION_COUNT) {
        return false;
    }
    // Play only the native attack art. combat_attack would reroll damage and
    // run scripts. Finish this presentation before the following checkpoint so
    // that an authoritative standing pose cannot cancel the attack halfway.
    if (worldMode == NetworkLaunchMode::Join
        && (actor->data.critter.combat.results & DAM_DEAD) == 0) {
        ScopedActingPlayerContext actingPlayer(*player, actor);
        Object* weapon = item_hit_with(actor, attack.hitMode);
        int weaponArt = weapon != nullptr && item_get_type(weapon) == ITEM_TYPE_WEAPON
            ? item_w_anim_code(weapon) : 0;
        int animation = item_w_anim(actor, attack.hitMode);
        int fid = art_id(OBJ_TYPE_CRITTER, actor->fid & 0xFFF,
            animation, weaponArt, actor->rotation + 1);
        if (art_exists(fid) && register_clear(actor) != -2) {
            combatActionResolving = true;
            Rect bounds;
            obj_set_rotation(actor, tile_dir(actor->tile, target->tile), &bounds);
            tile_refresh_rect(&bounds, actor->elevation);
            int readyFid = art_id(OBJ_TYPE_CRITTER, actor->fid & 0xFFF,
                ANIM_STAND, weaponArt, actor->rotation + 1);
            obj_change_fid(actor, readyFid, &bounds);
            tile_refresh_rect(&bounds, actor->elevation);
            if (register_begin(ANIMATION_REQUEST_RESERVED) == 0) {
                register_object_animate(actor, animation, 0);
                if (register_end() == 0) {
                    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                    while (anim_busy(actor) == -1 && std::chrono::steady_clock::now() < deadline) {
                        process_bk();
                        renderPresent();
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
            }
            register_clear(actor);
            obj_change_fid(actor, readyFid, &bounds);
            tile_refresh_rect(&bounds, actor->elevation);
            combatActionResolving = false;
        }
    }
    return true;
}

bool networkWorldApplyPeerSkillUse(const SkillUseStartedEvent& skillUse)
{
    if (!session.isActive() || !isValid(skillUse.skill)
        || (isInCombat() && skillUse.skill != ExplorationSkill::Sneak)
        || (skillUse.skill == ExplorationSkill::Sneak && skillUse.actorId != skillUse.targetId)) {
        return false;
    }
    if (skillUse.skill == ExplorationSkill::Steal && !skillUse.inventoryOpened) return true;
    PlayerCharacterState* player = session.players().findByActor(skillUse.actorId);
    Object* actor = player != nullptr ? session.entities().findObject(player->actorId) : nullptr;
    Object* target = session.entities().findObject(skillUse.targetId);
    if (player == nullptr
        || actor == nullptr
        || target == nullptr
        || actor->elevation != target->elevation
        || target->tile < 0) {
        return false;
    }

    // This is presentation-only on a replica. Calling action_use_skill_on or
    // obj_use_skill_on would rerun path callbacks, scripts, rolls, XP, and
    // target mutations. The next authoritative state carries those results.
    if (skillUse.skill == ExplorationSkill::Steal) {
        if (lootTargetIsInRange(actor, target))
            return networkWorldApplyPeerLoot(LootStartedEvent { skillUse.actorId, skillUse.targetId });
        if (isPlayerActor(target) || FID_TYPE(target->fid) != OBJ_TYPE_CRITTER
            || !critter_is_active(target) || obj_dist(actor, target) > 1) return false;
        bool opened = theftAccess.emplace(actor, TheftAccess { target }).second;
        activeLootTargets[actor] = target;
        if (opened && actor == localPlayerActor()) scripts_request_steal_container(actor, target);
    }
    if (skillUse.skill == ExplorationSkill::Sneak) register_clear(actor);
    return true;
}

bool networkWorldApplyPeerItemUse(const ItemUseStartedEvent& itemUse)
{
    if (!session.isActive() || isInCombat()) {
        return false;
    }
    PlayerCharacterState* player = session.players().findByActor(itemUse.actorId);
    Object* actor = player != nullptr ? session.entities().findObject(player->actorId) : nullptr;
    Object* target = session.entities().findObject(itemUse.targetId);
    Object* item = session.entities().findObject(itemUse.itemId);
    if (player == nullptr
        || actor == nullptr
        || target == nullptr
        || actor->elevation != target->elevation
        || target->tile < 0
        || (item != nullptr
            && (FID_TYPE(item->fid) != OBJ_TYPE_ITEM || topEnvironmentOrSelf(item) != actor))) {
        return false;
    }

    // The item use is only an ordered presentation boundary on replicas. The
    // target script, inventory consumption, rolls, variables, queue changes,
    // and XP arrive in the following authoritative state.
    return true;
}

bool networkWorldApplyPeerElevator(const ElevatorTransitionedEvent& elevator)
{
    Object* host = session.entities().findObject(session.playerActorId(kHostPlayerId));
    Object* guest = session.entities().findObject(session.playerActorId(kGuestPlayerId));
    if (!session.isActive()
        || host == nullptr
        || guest == nullptr
        || elevator.elevatorType < 0
        || elevator.elevatorType >= ELEVATOR_COUNT
        || elevator.map < 0
        || !hexGridTileIsValid(elevator.hostTile)
        || !elevationIsValid(elevator.hostElevation)
        || elevator.hostRotation < 0
        || elevator.hostRotation >= ROTATION_COUNT
        || !hexGridTileIsValid(elevator.guestTile)
        || !elevationIsValid(elevator.guestElevation)
        || elevator.guestRotation < 0
        || elevator.guestRotation >= ROTATION_COUNT
        || (elevator.hostElevation == elevator.guestElevation
            && elevator.hostTile == elevator.guestTile)
        || elevator.phaseRevision == 0) {
        return false;
    }

    bool mapChanged = elevator.map != map_data.field_34;
    if (mapChanged) {
        if (elevator.phaseRevision < 2
            || session.applyAuthoritativePhase(SessionPhase::Transition, elevator.phaseRevision - 1) != LocalSessionError::None
            || !loadSharedMap(elevator.map)) {
            return false;
        }
        host = session.entities().findObject(session.playerActorId(kHostPlayerId));
        guest = session.entities().findObject(session.playerActorId(kGuestPlayerId));
        if (host == nullptr || guest == nullptr || map_data.field_34 != elevator.map) {
            return false;
        }
    } else if (session.applyAuthoritativePhase(SessionPhase::Exploration, elevator.phaseRevision) != LocalSessionError::None) {
        return false;
    }

    std::vector<PlayerTransitionPlacement> placements {
        PlayerTransitionPlacement {
            kHostPlayerId, session.playerActorId(kHostPlayerId),
            elevator.hostTile, elevator.hostElevation, elevator.hostRotation,
        },
        PlayerTransitionPlacement {
            kGuestPlayerId, session.playerActorId(kGuestPlayerId),
            elevator.guestTile, elevator.guestElevation, elevator.guestRotation,
        },
    };
    Object* localActor = localPlayerActor();
    bool applied = applyPlayerPlacementRoster(placements)
        && localActor != nullptr
        && map_set_elevation(localActor->elevation) == 0
        && (!mapChanged || registerWorldObjects());
    return applied
        && (!mapChanged
            || session.applyAuthoritativePhase(SessionPhase::Exploration, elevator.phaseRevision) == LocalSessionError::None);
}

bool networkWorldApplyPeerExitGrid(const ExitGridTransitionedEvent& exitGrid)
{
    Object* source = session.entities().findObject(exitGrid.exitId);
    PlayerCharacterState* actingPlayer = session.players().findByActor(exitGrid.actorId);
    Object* actingActor = actingPlayer != nullptr
        ? session.entities().findObject(actingPlayer->actorId)
        : nullptr;
    int destinationMap = -1;
    int destinationTile = -1;
    int destinationElevation = -1;
    int destinationRotation = -1;
    if (!session.isActive()
        || source == nullptr
        || actingActor == nullptr
        || !exitGridDestination(source,
            destinationMap,
            destinationTile,
            destinationElevation,
            destinationRotation)
        || destinationMap != exitGrid.map
        || exitGrid.phaseRevision < 2
        || exitGrid.placements.size() != session.players().size()
        || session.applyAuthoritativePhase(SessionPhase::Transition, exitGrid.phaseRevision - 1) != LocalSessionError::None
        || !loadSharedMap(exitGrid.map)) {
        std::fprintf(stderr, "Multiplayer exit-grid replica failed before destination placement.\n");
        return false;
    }

    Object* localActor = localPlayerActor();
    bool applied = applyPlayerPlacementRoster(exitGrid.placements)
        && localActor != nullptr
        && map_set_elevation(localActor->elevation) == 0
        && registerWorldObjects()
        && session.applyAuthoritativePhase(SessionPhase::Exploration, exitGrid.phaseRevision) == LocalSessionError::None;
    if (!applied) {
        std::fprintf(stderr, "Multiplayer exit-grid replica failed destination registration or phase completion.\n");
    }
    return applied;
}

bool networkWorldApplyPeerWorldMapArrival(const WorldMapArrivedEvent& arrival)
{
    if (!session.isActive()
        || session.phase() != SessionPhase::Transition
        || !activeSharedModal.has_value()
        || activeSharedModal->kind != SharedModalKind::WorldMap
        || activeSharedModal->actorId != arrival.actorId
        || arrival.phaseRevision != session.phaseRevision() + 1
        || arrival.placements.size() != session.players().size()
        || arrival.map < 0) {
        return false;
    }
    WorldMapState position;
    worldmap_capture_state(position);
    position.x = arrival.worldX;
    position.y = arrival.worldY;
    if (!worldmap_apply_state(position)) return false;
    set_game_time(arrival.gameTime);
    if (arrival.kind == WorldMapArrivalKind::Fatal) {
        if (arrival.map != map_data.field_34) return false;
        if (!applyPlayerPlacementRoster(arrival.placements)
            || session.applyAuthoritativePhase(SessionPhase::Exploration, arrival.phaseRevision)
                != LocalSessionError::None) {
            return false;
        }
        activeSharedModal.reset();
        approvedWorldMapProposerActorId = {};
        approvedWorldMapProposalSequence = {};
        selectedWorldMapRoute.reset();
        game_user_wants_to_quit = 1;
        return true;
    }
    game_global_vars[GVAR_LOAD_MAP_INDEX] = arrival.entranceIndex;
    if (!loadSharedMap(arrival.map)) return false;

    Object* localActor = localPlayerActor();
    bool validRoster = applyPlayerPlacementRoster(arrival.placements) && localActor != nullptr;
    bool validElevation = validRoster && map_set_elevation(localActor->elevation) == 0;
    bool registered = validElevation && registerWorldObjects();
    bool advanced = registered
        && session.applyAuthoritativePhase(SessionPhase::Exploration, arrival.phaseRevision)
            == LocalSessionError::None;
    if (!advanced) {
        std::fprintf(stderr,
            "Multiplayer world-map arrival replica failed roster=%d elevation=%d registration=%d phase=%d revision=%u.\n",
            validRoster, validElevation, registered,
            static_cast<int>(session.phase()), arrival.phaseRevision);
    }
    return advanced;
}

bool networkWorldApplyPeerSceneryTransition(const SceneryTransitionedEvent& transition)
{
    Object* source = session.entities().findObject(transition.transitionId);
    PlayerCharacterState* actingPlayer = session.players().findByActor(transition.actorId);
    Object* actingActor = actingPlayer != nullptr
        ? session.entities().findObject(actingPlayer->actorId)
        : nullptr;
    int sceneryType = -1;
    bool mapChanged = transition.map != map_data.field_34;
    if (!session.isActive()
        || source == nullptr
        || actingActor == nullptr
        || !sceneryTransitionType(source, sceneryType)
        || transition.phaseRevision < 2
        || transition.placements.size() != session.players().size()
        || session.applyAuthoritativePhase(SessionPhase::Transition, transition.phaseRevision - 1) != LocalSessionError::None
        || (mapChanged && !loadSharedMap(transition.map))) {
        return false;
    }

    Object* localActor = localPlayerActor();
    return applyPlayerPlacementRoster(transition.placements)
        && localActor != nullptr
        && map_set_elevation(localActor->elevation) == 0
        && (!mapChanged || registerWorldObjects())
        && session.applyAuthoritativePhase(SessionPhase::Exploration, transition.phaseRevision) == LocalSessionError::None;
}

bool networkWorldApplyPeerRest(const RestStateChangedEvent& rest)
{
    PlayerCharacterState* player = session.players().findByActor(rest.actorId);
    if (!session.isActive()
        || player == nullptr
        || !isValidRestMinutes(rest.minutes)
        || rest.gameTime <= 0) {
        return false;
    }
    if (rest.completed) {
        if (rest.minutes == 0
            || rest.phaseRevision < 2
            || rest.gameTime < game_time()
            || session.applyAuthoritativePhase(SessionPhase::Transition, rest.phaseRevision - 1) != LocalSessionError::None) {
            return false;
        }
        pendingRestProposal.reset();
        set_game_time(rest.gameTime);
        if (rest.interrupted) {
            char message[] = "Shared rest was interrupted.";
            display_print(message);
        }
        return session.applyAuthoritativePhase(SessionPhase::Exploration, rest.phaseRevision) == LocalSessionError::None;
    }
    if (rest.phaseRevision != session.phaseRevision()
        || session.phase() != SessionPhase::Exploration) {
        return false;
    }
    if (rest.minutes == 0) {
        if (pendingRestProposal.has_value()) {
            pendingRestProposal->readyPlayers.erase(player->id);
            if (pendingRestProposal->proposer == player->id
                || pendingRestProposal->readyPlayers.empty()) {
                pendingRestProposal.reset();
            }
        }
    } else {
        if (!pendingRestProposal.has_value()
            || pendingRestProposal->minutes != rest.minutes) {
            pendingRestProposal = PendingRestProposal {
                rest.minutes,
                player->id,
                map_data.field_34,
                session.phaseRevision(),
                std::chrono::steady_clock::now() + kRestProposalLifetime,
                {},
            };
        }
        pendingRestProposal->readyPlayers.insert(player->id);
    }
    return true;
}

int networkWorldPendingRestMinutes()
{
    if (!pendingRestProposal.has_value()
        || pendingRestProposal->expiresAt <= std::chrono::steady_clock::now()
        || pendingRestProposal->map != map_data.field_34
        || pendingRestProposal->phaseRevision != session.phaseRevision()) {
        return 0;
    }
    return pendingRestProposal->minutes;
}

std::string networkWorldPendingRestProposerName()
{
    if (networkWorldPendingRestMinutes() == 0) {
        return {};
    }
    const PlayerCharacterState* player = session.players().find(pendingRestProposal->proposer);
    return player != nullptr ? player->name : std::string {};
}

bool networkWorldLocalRestProposal()
{
    PlayerId localPlayerId = worldMode == NetworkLaunchMode::Host
        ? kHostPlayerId
        : kGuestPlayerId;
    return networkWorldPendingRestMinutes() != 0
        && pendingRestProposal->proposer == localPlayerId;
}

PlayerId networkWorldPendingRestProposer()
{
    return networkWorldPendingRestMinutes() != 0
        ? pendingRestProposal->proposer
        : PlayerId {};
}

void networkWorldClearRestProposal()
{
    pendingRestProposal.reset();
}

bool networkWorldCaptureScriptedMapTransition(const MapTransition& transition)
{
    if (!scriptedSceneryTransitionInProgress) {
        if (worldMode == NetworkLaunchMode::Host && session.isActive()
            && session.phase() == SessionPhase::Dialogue
            && activeSharedModal.has_value()
            && activeSharedModal->kind == SharedModalKind::Dialogue
            && transition.map > 0 && dialogueCause.value != 0) {
            if (!pendingDialogueMapTransition.has_value()) {
                pendingDialogueMapTransition = PendingDialogueMapTransition {
                    transition, activeSharedModal->actorId, dialogueCause };
            }
            return true;
        }
        return false;
    }
    // A script may request departure more than once. Keep the first request
    // authoritative and prevent later requests from reaching local map state.
    if (!capturedSceneryMapTransition.has_value()) {
        capturedSceneryMapTransition = transition;
    }
    return true;
}

Object* networkWorldScriptedSceneryTransitionActor(Object* requestedActor)
{
    return scriptedSceneryTransitionInProgress && requestedActor == obj_dude
        ? actingPlayerActorOr(requestedActor)
        : requestedActor;
}

static bool runNpcBarterSmokeTest()
{
    Object* actor = networkWorldPlayerActor(kGuestPlayerId);
    auto* player = playerStateForActor(actor);
    if (player == nullptr || session.phase() != SessionPhase::Exploration) return false;
    CharacterBuild savedBuild = player->build;
    std::size_t eventStart = deferredEvents.size();
    Object* seller = nullptr;
    Object* payment = nullptr;
    Object* stock = nullptr;
    Object* reserved = nullptr;
    Proto* proto = nullptr;
    int oldFlags = 0;
    auto countPid = [](Object* owner, int pid) {
        int count = 0;
        for (int i = 0; i < owner->data.inventory.length; ++i) {
            auto& entry = owner->data.inventory.items[i];
            if (entry.item->pid == pid) count += entry.quantity;
        }
        return count;
    };
    auto findPid = [](Object* owner, int pid) -> Object* {
        for (int i = 0; i < owner->data.inventory.length; ++i) if (owner->data.inventory.items[i].item->pid == pid) return owner->data.inventory.items[i].item;
        return nullptr;
    };
    int baselineStock = countPid(actor, PROTO_ID_STIMPACK);
    int baselineCaps = item_caps_total(actor);
    int baselinePayment = countPid(actor, PROTO_ID_FLARE);
    bool passed = false;
    if (obj_pid_new(&seller, 0x1000000) == 0 && seller != nullptr
        && obj_move_to_tile(seller, actor->tile, actor->elevation, nullptr) == 0 && session.registerWorldObject(seller)
        && proto_ptr(seller->pid, &proto) == 0
        && obj_pid_new(&payment, PROTO_ID_FLARE) == 0 && obj_disconnect(payment, nullptr) == 0 && item_add_force(actor, payment, 3) == 0
        && obj_pid_new(&stock, PROTO_ID_STIMPACK) == 0 && obj_disconnect(stock, nullptr) == 0 && item_add_force(seller, stock, 3) == 0
        && obj_pid_new(&reserved, 8) == 0 && obj_disconnect(reserved, nullptr) == 0 && item_add_force(seller, reserved, 1) == 0
        && item_caps_adjust(seller, 100) == 0 && registerUntrackedInventory(actor) && registerUntrackedInventory(seller)) {
        oldFlags = proto->critter.data.flags;
        proto->critter.data.flags |= CRITTER_BARTER;
        ScopedActingPlayerContext context(*player, actor);
        ScopedPlayerFeedback feedback(actor);
        player->build.skillPoints[SKILL_BARTER] = 100;
        auto buyerId = player->actorId;
        auto sellerId = session.entities().findEntity(seller).value_or(EntityId {});
        auto paymentId = session.entities().findEntity(payment).value_or(EntityId {});
        auto stockId = session.entities().findEntity(stock).value_or(EntityId {});
        session.transitionTo(SessionPhase::Dialogue);
        activeSharedModal = ActiveSharedModal { buyerId, SharedModalKind::Dialogue };
        dialoguePresentation = DialoguePresentationEvent {};
        dialoguePresentation->actorId = buyerId; dialoguePresentation->targetId = sellerId;
        auto command = [&](NpcBarterAction action, DirectTradeOffer buyer = {}, DirectTradeOffer requested = {}) {
            return commandExecutor.npcBarter(actor, NpcBarterCommand { action, sellerId,
                action == NpcBarterAction::Begin ? 0 : npcBarterState->revision, buyer, requested });
        };
        proto->critter.data.flags &= ~CRITTER_BARTER;
        bool refuses = command(NpcBarterAction::Begin).status == CommandExecutionStatus::InvalidAction && !npcBarterState.has_value();
        proto->critter.data.flags |= CRITTER_BARTER;
        bool began = command(NpcBarterAction::Begin).status == CommandExecutionStatus::Applied;
        bool reservedHidden = !npcBarterItemAvailable(seller, reserved, true);
        auto staleRevision = npcBarterState->revision;
        bool badQuantity = command(NpcBarterAction::Offer, {}, DirectTradeOffer { { { stockId, 4 } }, 0 }).status
            == CommandExecutionStatus::InvalidAction;
        payment->flags |= OBJECT_IN_RIGHT_HAND;
        bool equipped = command(NpcBarterAction::Offer, DirectTradeOffer { { { paymentId, 1 } }, 0 }, {}).status
            == CommandExecutionStatus::InvalidAction;
        payment->flags &= ~OBJECT_IN_RIGHT_HAND;
        bool staged = command(NpcBarterAction::Offer, DirectTradeOffer { { { paymentId, 1 } }, 0 },
            DirectTradeOffer { { { stockId, 3 } }, 0 }).status == CommandExecutionStatus::Applied;
        bool noStagingMoves = item_count(seller, stock) == 3 && countPid(actor, PROTO_ID_STIMPACK) == baselineStock;
        bool unauthorized = commandExecutor.npcBarter(networkWorldPlayerActor(kHostPlayerId),
            NpcBarterCommand { NpcBarterAction::Accept, sellerId, npcBarterState->revision }).status == CommandExecutionStatus::InvalidAction;
        bool stale = commandExecutor.npcBarter(actor,
            NpcBarterCommand { NpcBarterAction::Accept, sellerId, staleRevision }).status == CommandExecutionStatus::InvalidAction;
        bool rejected = command(NpcBarterAction::Accept).state.status == NpcBarterStatus::Rejected
            && item_count(seller, stock) == 3 && countPid(actor, PROTO_ID_STIMPACK) == baselineStock;
        auto paymentQuantity = static_cast<unsigned>(item_count(actor, payment));
        bool fair = command(NpcBarterAction::Offer, DirectTradeOffer { { { paymentId, paymentQuantity } }, 0 },
            DirectTradeOffer { { { stockId, 1 } }, 7 }).status == CommandExecutionStatus::Applied;
        auto goodPrice = npcBarterState->askingValue;
        player->build.skillPoints[SKILL_BARTER] = 0;
        priceNpcBarter(actor, seller, *npcBarterState);
        bool skillPrice = npcBarterState->askingValue > goodPrice;
        player->build.skillPoints[SKILL_BARTER] = 100;
        stock->flags |= OBJECT_IN_RIGHT_HAND;
        bool revalidated = command(NpcBarterAction::Accept).state.status == NpcBarterStatus::Rejected
            && countPid(actor, PROTO_ID_STIMPACK) == baselineStock;
        stock->flags &= ~OBJECT_IN_RIGHT_HAND;
        command(NpcBarterAction::Offer, DirectTradeOffer { { { paymentId, paymentQuantity } }, 0 },
            DirectTradeOffer { { { stockId, 1 } }, 7 });
        auto committed = command(NpcBarterAction::Accept);
        bool exchanged = committed.state.status == NpcBarterStatus::Committed && !npcBarterState.has_value()
            && countPid(actor, PROTO_ID_STIMPACK) == baselineStock + 1 && item_caps_total(actor) == baselineCaps + 7
            && countPid(seller, PROTO_ID_STIMPACK) == 2 && item_caps_total(seller) == 93;
        bool reopened = command(NpcBarterAction::Begin).status == CommandExecutionStatus::Applied;
        bool cancelled = command(NpcBarterAction::Cancel).state.status == NpcBarterStatus::Cancelled && !npcBarterState.has_value();
        auto presentation = *dialoguePresentation;
        dialoguePresentation.reset();
        bool scriptedQueued = networkWorldRequestScriptedNpcBarter(seller);
        networkWorldProcessScriptedNpcBarter();
        bool waitedForPresentation = !npcBarterState.has_value();
        dialoguePresentation = presentation;
        networkWorldProcessScriptedNpcBarter();
        bool scriptedGuest = npcBarterState.has_value() && npcBarterState->buyerId == buyerId;
        networkWorldDirectTradeSetConnected(kHostPlayerId, false);
        bool observerDeparture = npcBarterState.has_value();
        networkWorldDirectTradeSetConnected(kGuestPlayerId, false);
        bool buyerDeparture = !npcBarterState.has_value() && !deferredEvents.empty()
            && std::get_if<NpcBarterStateChangedEvent>(&deferredEvents.back().payload) != nullptr
            && std::get<NpcBarterStateChangedEvent>(deferredEvents.back().payload).state.status == NpcBarterStatus::Cancelled;
        passed = refuses && began && reservedHidden && badQuantity && equipped && staged && noStagingMoves
            && unauthorized && stale && rejected && fair && skillPrice && revalidated && exchanged && reopened && cancelled && scriptedQueued && waitedForPresentation && scriptedGuest && observerDeparture && buyerDeparture;
        std::fprintf(stderr, "NATIVE_NPC_BARTER_%s begin=%d reserved=%d invalid=%d staged=%d owner=%d stale=%d reject=%d commit=%d cancel=%d observer=%d disconnect=%d\n",
            passed ? "PASS" : "FAIL", began, reservedHidden, badQuantity && equipped, staged && noStagingMoves,
            unauthorized, stale, rejected, exchanged, cancelled, observerDeparture, buyerDeparture);
        proto->critter.data.flags = oldFlags;
    }
    npcBarterState.reset();
    pendingScriptedNpcBarter.reset();
    dialoguePresentation.reset(); dialogueVotes.clear(); activeSharedModal.reset();
    if (session.phase() == SessionPhase::Dialogue) session.transitionTo(SessionPhase::Exploration);
    player->build = savedBuild;
    while (deferredEvents.size() > eventStart) deferredEvents.pop_back();
    int excessStock = countPid(actor, PROTO_ID_STIMPACK) - baselineStock;
    if (excessStock > 0) {
        Object* item = findPid(actor, PROTO_ID_STIMPACK);
        if (item != nullptr && item_remove_mult(actor, item, excessStock) == 0) obj_destroy(item);
    }
    item_caps_adjust(actor, baselineCaps - item_caps_total(actor));
    // The fixture may merge its flares with the player's baseline stack.
    int missingPayment = baselinePayment - countPid(actor, PROTO_ID_FLARE);
    if (missingPayment > 0) {
        Object* restored = nullptr;
        if (obj_pid_new(&restored, PROTO_ID_FLARE) == 0) { obj_disconnect(restored, nullptr); item_add_force(actor, restored, missingPayment); }
    } else if (missingPayment < 0) {
        Object* item = findPid(actor, PROTO_ID_FLARE);
        if (item != nullptr && item_remove_mult(actor, item, -missingPayment) == 0) obj_destroy(item);
    }
    if (seller != nullptr) {
        while (seller->data.inventory.length > 0) {
            Object* item = seller->data.inventory.items[0].item;
            item_remove_mult(seller, item, item_count(seller, item)); obj_destroy(item);
        }
        obj_erase_object(seller, nullptr);
    }
    registerUntrackedInventory(actor);
    return passed;
}

// Execute real interpreter opcodes inside host-owned USE_SKILL_ON and PICKUP fixtures.
// The bytecode is deliberately small so it needs no external script compiler.
static bool runTheftScriptAndSneakSmoke()
{
    std::array<int, 4> globals;
    for (int index = 0; index < 4; ++index) globals[index] = game_get_global_var(index);
    bool passed = true;
    std::size_t eventStart = deferredEvents.size();
    for (PlayerId id : session.players().playerIds()) {
        Object* actor = networkWorldPlayerActor(id);
        auto* player = playerStateForActor(actor);
        if (actor == nullptr || player == nullptr) { passed = false; break; }
        CharacterBuild savedBuild = player->build;
        // Suppress a native UI request during this executor fixture.
        PlayerId otherId;
        for (PlayerId candidate : session.players().playerIds()) if (candidate != id) { otherId = candidate; break; }
        ScopedLocalPlayerBinding otherLocal(session, otherId);
        {
            ScopedActingPlayerContext context(*player, actor);
            player->build.prototypeFlags |= 1 << PC_FLAG_SNEAKING;
            for (PlayerId queriedId : session.players().playerIds()) {
                Object* queried = networkWorldPlayerActor(queriedId);
                auto* queriedPlayer = playerStateForActor(queried);
                bool expected = (queriedPlayer->build.prototypeFlags & (1 << PC_FLAG_SNEAKING)) != 0;
                passed = intExtraUsingSkill(queried, SKILL_SNEAK) == expected
                    && actingPlayerActor() == actor && passed;
            }
            passed = !intExtraUsingSkill(nullptr, SKILL_SNEAK)
                && !intExtraUsingSkill(actor, SKILL_STEAL) && passed;
            for (int testCase = 0; testCase < 10; ++testCase) {
                bool ordinaryLoot = testCase >= 5;
                int mode = testCase % 5;
                Object* target = nullptr;
                int sid = -1;
                if (obj_pid_new(&target, mode == 4 ? 213 : 0x1000000) != 0 || target == nullptr
                    || obj_move_to_tile(target, actor->tile, actor->elevation, nullptr) != 0
                    || !session.registerWorldObject(target) || scr_new(&sid, SCRIPT_TYPE_CRITTER) != 0) {
                    if (target != nullptr) obj_erase_object(target, nullptr);
                    passed = false;
                    break;
                }
                auto targetId = session.entities().findEntity(target).value_or(EntityId {});
                target->sid = sid;
                if (ordinaryLoot && mode != 4) target->data.critter.combat.results |= DAM_DEAD;
                Program program {};
                char name[] = "multiplayer-theft-fixture";
                ProgramStack stack, returns;
                std::vector<unsigned char> data(64, 0);
                std::array<unsigned char, 64> procedures {};
                // Procedure index 1 begins at byte 64, big endian address.
                procedures[4 + sizeof(Procedure) + 19] = 64;
                auto op = [&](int value) { data.push_back(value >> 8); data.push_back(value & 255); };
                auto number = [&](int value) {
                    op(VALUE_TYPE_INT);
                    for (int shift = 24; shift >= 0; shift -= 8) data.push_back((value >> shift) & 255);
                };
                op(OPCODE_POP); // zero procedure arguments
                number(0); number(0); op(0x80C5); number(1); op(OPCODE_ADD); op(0x80C6);
                number(1); op(0x80BD); number(SKILL_SNEAK); op(0x80AB); op(0x80C6);
                number(2); op(0x80BD); op(0x80BF); op(OPCODE_EQUAL); op(0x80C6);
                number(3); op(0x80FA); op(0x80C6);
                if (mode == 1) op(0x80B9); // script_overrides
                if (mode == 2) {
                    op(0x80BC); number(tile_num_in_direction(actor->tile, ROTATION_SE, 6));
                    number(actor->elevation); op(0x80B6); op(OPCODE_POP);
                }
                if (mode == 3) { op(0x80BC); op(0x80F4); } // destroy self
                op(OPCODE_POP_FLAGS_EXIT);
                program.name = name; program.data = data.data(); program.procedures = procedures.data();
                program.stackValues = &stack; program.returnStackValues = &returns;
                Script* script = nullptr;
                bool initialized = scr_ptr(sid, &script) == 0;
                if (initialized) {
                    script->owner = target; script->program = &program; script->scr_flags |= SCRIPT_FLAG_0x01;
                    script->procs[SCRIPT_PROC_USE_SKILL_ON] = 1;
                    script->procs[SCRIPT_PROC_PICKUP] = 1;
                }
                for (int index = 0; index < 4; ++index) game_set_global_var(index, 0);
                if (mode == 4) {
                    if (ordinaryLoot) obj_lock(target);
                    else obj_jam_lock(target);
                }
                engineExecutionProbeBegin();
                auto result = !initialized ? CommandExecutionStatus::InvalidAction
                    : ordinaryLoot ? commandExecutor.loot(actor, target)
                    : commandExecutor.useSkill(actor, target, UseSkillCommand { targetId, ExplorationSkill::Steal });
                auto counts = engineExecutionProbeEnd();
                bool exists = session.entities().findObject(targetId) == target;
                bool opened = ordinaryLoot
                    ? activeLootTargets.count(actor) != 0 && activeLootTargets.at(actor) == target
                    : commandExecutor.skillInventoryOpened(actor, target);
                bool expectedOpen = mode == 0;
                bool outcome = (ordinaryLoot ? mode == 0 : mode < 2) ? result == CommandExecutionStatus::Applied
                    : result == CommandExecutionStatus::InvalidAction;
                bool effects = mode == 4 ? counts.scriptProcedures == 0 && game_get_global_var(0) == 0
                    : counts.scriptProcedures == 1 && game_get_global_var(0) == 1
                        && game_get_global_var(1) == 1 && game_get_global_var(2) == 1
                        && (ordinaryLoot || game_get_global_var(3) == SKILL_STEAL);
                bool npcRejected = !exists || !intExtraUsingSkill(target, SKILL_SNEAK);
                if (opened && !ordinaryLoot) {
                    theftAccess.at(actor).attempts = 1;
                    finishTheft(actor, true);
                    effects = game_get_global_var(0) == 2 && effects;
                }
                if (ordinaryLoot) activeLootTargets.erase(actor);
                bool clean = !networkWorldHasTheftAccess(actor);
                bool casePassed = outcome && effects && opened == expectedOpen && clean && npcRejected
                    && (mode != 3 || !exists);
                std::fprintf(stderr, "NATIVE_%s_SCRIPT_%s player=%u mode=%d hooks=%u open=%d effect=%d clean=%d\n",
                    ordinaryLoot ? "LOOT" : "THEFT", casePassed ? "PASS" : "FAIL", id.value, mode, counts.scriptProcedures, opened, effects, clean);
                passed = casePassed && passed;
                if (exists) {
                    // Detach the stack-owned program before destroying its script.
                    if (scr_ptr(sid, &script) == 0) script->program = nullptr;
                    obj_erase_object(target, nullptr);
                }
            }
        }
        player->build = savedBuild;
    }
    for (int index = 0; index < 4; ++index) game_set_global_var(index, globals[index]);
    while (deferredEvents.size() > eventStart) deferredEvents.pop_back();
    std::fprintf(stderr, "NATIVE_SCRIPT_SNEAK_%s explicit_players=1 npc_rejected=1 scope_restored=1\n", passed ? "PASS" : "FAIL");
    return passed;
}

static bool runTheftTransactionSmokeTest()
{
    Object* actor = networkWorldPlayerActor(kGuestPlayerId);
    PlayerCharacterState* player = playerStateForActor(actor);
    if (player == nullptr) return false;
    CharacterBuild savedBuild = player->build;
    int savedHitPoints = actor->data.critter.hp;
    std::size_t eventStart = deferredEvents.size();
    Object* target = nullptr;
    Object* item = nullptr;
    bool passed = false;
    // An unowned biped fixture, unlike a rat which cannot accept planted items.
    if (obj_pid_new(&target, 0x1000000) == 0 && target != nullptr
        && obj_move_to_tile(target, actor->tile, actor->elevation, nullptr) == 0
        && session.registerWorldObject(target)
        && obj_pid_new(&item, kAntidotePid) == 0 && item != nullptr
        && item_add_force(target, item, 3) == 0 && registerItem(item)) {
        ScopedActingPlayerContext context(*player, actor);
        ScopedPlayerFeedback feedback(actor);
        auto targetId = session.entities().findEntity(target).value_or(EntityId {});
        auto actorId = player->actorId;
        auto quantity = [](Object* holder) {
            int result = 0;
            for (int index = 0; index < holder->data.inventory.length; ++index) {
                const auto& entry = holder->data.inventory.items[index];
                if (entry.item->pid == kAntidotePid) result += entry.quantity;
            }
            return result;
        };
        auto open = [&]() {
            return commandExecutor.useSkill(actor, target, UseSkillCommand { targetId, ExplorationSkill::Steal })
                == CommandExecutionStatus::Applied;
        };
        auto close = [&]() {
            return commandExecutor.setSharedModal(actor, SharedModalCommand { SharedModalKind::Theft, false }).status
                == CommandExecutionStatus::Applied;
        };
        auto transfer = [&](Object* source, Object* destination, unsigned available) {
            return commandExecutor.transferInventory(actor, source, destination, item, InventoryTransferCommand {
                session.entities().findEntity(source).value_or(EntityId {}),
                session.entities().findEntity(destination).value_or(EntityId {}),
                session.entities().findEntity(item).value_or(EntityId {}), 1, available, {} }).status;
        };
        auto seedFor = [&](int outcome, bool planting) {
            int savedCount = gStealCount;
            gStealCount = theftAccess.count(actor) != 0 ? theftAccess.at(actor).attempts + 1 : 1;
            int selected = -1;
            for (int seed = 1; seed <= 100 && selected < 0; ++seed) {
                roll_set_seed(seed);
                if (skill_check_stealing(actor, target, item, planting) == outcome) selected = seed;
            }
            gStealCount = savedCount;
            return selected;
        };
        player->build.skillPoints[SKILL_STEAL] = 100;
        player->build.perkRanks[PERK_SWIFT_LEARNER] = 0;
        int initialXp = player->build.experience;
        bool opened = open();
        engineExecutionProbeBegin();
        bool staleRejected = transfer(target, actor, 4) == CommandExecutionStatus::InvalidAction;
        item->flags |= OBJECT_IN_RIGHT_HAND;
        bool equippedRejected = transfer(target, actor, 3) == CommandExecutionStatus::InvalidAction;
        item->flags &= ~OBJECT_IN_RIGHT_HAND;
        auto rejectedCounts = engineExecutionProbeEnd();
        int takeSeed = seedFor(1, false);
        roll_set_seed(takeSeed);
        engineExecutionProbeBegin();
        bool taken = takeSeed > 0 && transfer(target, actor, 3) == CommandExecutionStatus::Applied
            && quantity(target) == 2 && item->owner == actor;
        auto successCounts = engineExecutionProbeEnd();
        int plantSeed = seedFor(1, true);
        roll_set_seed(plantSeed);
        bool planted = plantSeed > 0 && transfer(actor, target, 1) == CommandExecutionStatus::Applied
            && quantity(target) == 3 && item->owner == target;
        std::size_t beforeClose = deferredEvents.size();
        bool closed = close() && !networkWorldHasTheftAccess(actor)
            && player->build.experience == initialXp + 30;
        bool xpFeedback = std::any_of(deferredEvents.begin() + beforeClose, deferredEvents.end(),
            [actorId](const GameEvent& event) {
                const auto* feedback = std::get_if<PlayerFeedbackEvent>(&event.payload);
                return feedback != nullptr && feedback->actorId == actorId && !feedback->text.empty();
            });
        player->build.skillPoints[SKILL_STEAL] = 0;
        int caughtXp = player->build.experience;
        bool reopened = open();
        int caughtSeed = seedFor(0, false);
        roll_set_seed(caughtSeed);
        engineExecutionProbeBegin();
        bool caught = caughtSeed > 0 && transfer(target, actor, 3) == CommandExecutionStatus::InvalidAction
            && !networkWorldHasTheftAccess(actor) && quantity(target) == 3
            && player->build.experience == caughtXp;
        auto caughtCounts = engineExecutionProbeEnd();
        int originalTile = actor->tile;
        int originalFid = actor->fid;
        bool weaponArt = false;
        for (int weapon = 1; weapon <= 10 && !weaponArt; ++weapon) {
            int standing = art_id(FID_TYPE(originalFid), originalFid & 0xFFF, ANIM_STAND, weapon, actor->rotation + 1);
            int walking = art_id(FID_TYPE(originalFid), originalFid & 0xFFF, ANIM_WALK, weapon, actor->rotation + 1);
            if (art_exists(standing) && art_exists(walking))
                weaponArt = obj_change_fid(actor, standing, nullptr) == 0;
        }
        bool approached = false;
        for (int rotation = 0; rotation < ROTATION_COUNT && !approached; ++rotation) {
            int remoteTile = tile_num_in_direction(target->tile, rotation, 6);
            std::array<unsigned char, kMaximumMovementPathLength> path;
            if (!hexGridTileIsValid(remoteTile) || obj_blocking_at(actor, remoteTile, actor->elevation) != nullptr
                || make_path(actor, originalTile, remoteTile, path.data(), 1) <= 0) continue;
            if (obj_move_to_tile(actor, remoteTile, actor->elevation, nullptr) == 0) {
                bool wasDistant = obj_dist(actor, target) > 1;
                approached = wasDistant && open() && obj_dist(actor, target) <= 1 && close();
                register_clear(actor);
                obj_move_to_tile(actor, originalTile, actor->elevation, nullptr);
            }
        }
        obj_change_fid(actor, originalFid, nullptr);
        int fallbackXp = player->build.experience;
        target->data.critter.combat.results |= DAM_KNOCKED_OUT;
        bool unconsciousLoot = open() && !networkWorldHasTheftAccess(actor)
            && activeLootTargets.count(actor) != 0 && activeLootTargets.at(actor) == target;
        activeLootTargets.erase(actor);
        target->data.critter.combat.results = DAM_DEAD;
        bool corpseLoot = open() && !networkWorldHasTheftAccess(actor)
            && activeLootTargets.count(actor) != 0 && activeLootTargets.at(actor) == target
            && player->build.experience == fallbackXp;
        activeLootTargets.erase(actor);
        Object* container = nullptr;
        bool containerLoot = false;
        bool lockedRejected = false;
        if (obj_pid_new(&container, 213) == 0 && container != nullptr
            && obj_move_to_tile(container, originalTile, actor->elevation, nullptr) == 0
            && session.registerWorldObject(container)) {
            auto containerId = session.entities().findEntity(container).value_or(EntityId {});
            containerLoot = commandExecutor.useSkill(actor, container, UseSkillCommand { containerId, ExplorationSkill::Steal })
                    == CommandExecutionStatus::Applied
                && !networkWorldHasTheftAccess(actor) && activeLootTargets.count(actor) != 0
                && activeLootTargets.at(actor) == container;
            activeLootTargets.erase(actor);
            obj_lock(container);
            lockedRejected = commandExecutor.useSkill(actor, container, UseSkillCommand { containerId, ExplorationSkill::Steal })
                    == CommandExecutionStatus::InvalidAction
                && activeLootTargets.count(actor) == 0;
        }
        if (container != nullptr) obj_erase_object(container, nullptr);
        target->data.critter.combat.results = 0;
        passed = opened && staleRejected && equippedRejected && rejectedCounts.randomDraws == 0
            && taken && successCounts.randomDraws > 0 && planted && closed && xpFeedback && reopened && caught
            && caughtCounts.randomDraws > 0 && approached && weaponArt && unconsciousLoot && corpseLoot && containerLoot && lockedRejected;
        std::fprintf(stderr, "NATIVE_THEFT_APPROACH_%s movement=%d weapon_art=%d unconscious=%d corpse=%d container=%d locked=%d\n",
            approached && weaponArt && unconsciousLoot && corpseLoot && containerLoot && lockedRejected ? "PASS" : "FAIL",
            approached, weaponArt, unconsciousLoot, corpseLoot, containerLoot, lockedRejected);
        std::fprintf(stderr, "NATIVE_THEFT_TRANSACTIONS_%s open=%d stale=%d equipped=%d take=%d plant=%d close_xp=%d caught=%d host_rng=%u\n",
            passed ? "PASS" : "FAIL", opened, staleRejected, equippedRejected, taken, planted, closed, caught,
            successCounts.randomDraws + caughtCounts.randomDraws);
    }
    theftAccess.erase(actor);
    activeLootTargets.erase(actor);
    player->build = savedBuild;
    actor->data.critter.hp = savedHitPoints;
    while (deferredEvents.size() > eventStart) deferredEvents.pop_back();
    // Split representatives are registered separately and may have merged.
    std::vector<Object*> temporaryItems;
    for (const auto& entry : worldItems) {
        if ((target != nullptr && entry.second->owner == target) || (entry.second == item && entry.second->owner == actor))
            temporaryItems.push_back(entry.second);
    }
    for (Object* temporary : temporaryItems) {
        if (temporary->owner != nullptr) {
            Object* owner = temporary->owner;
            item_remove_mult(owner, temporary, item_count(owner, temporary));
        }
        obj_destroy(temporary);
    }
    if (target != nullptr) {
        theftAccess.emplace(actor, TheftAccess { target });
        activeLootTargets[actor] = target;
        obj_erase_object(target, nullptr);
        bool cleaned = !networkWorldHasTheftAccess(actor)
            && activeLootTargets.count(actor) == 0 && !session.entities().findEntity(target).has_value();
        std::fprintf(stderr, "NATIVE_THEFT_DESTROYED_TARGET_%s access_closed=%d registry_removed=%d\n",
            cleaned ? "PASS" : "FAIL", !networkWorldHasTheftAccess(actor), !session.entities().findEntity(target).has_value());
        passed = passed && cleaned;
    }
    return passed;
}

bool networkWorldPrepareCharacterEditorSmoke()
{
    if (worldMode != NetworkLaunchMode::Host || !session.isActive()) return false;
    for (PlayerId id : session.players().playerIds()) {
        auto* player = session.players().find(id);
        Object* actor = networkWorldPlayerActor(id);
        if (player == nullptr || actor == nullptr) return false;
        player->build.baseStats[STAT_PERCEPTION] = 6;
        player->build.baseStats[STAT_INTELLIGENCE] = 6;
        player->build.level = player->build.processedLevel = 3;
        player->build.experience = 3000;
        player->build.unspentSkillPoints = 10;
        player->build.pendingPerks = 1;
        player->build.skillPoints.fill(0);
        player->build.perkRanks.fill(0);
        player->build.traits = { -1, -1 };
        player->build.taggedSkills = { SKILL_SMALL_GUNS, SKILL_FIRST_AID, SKILL_REPAIR, -1 };
        ScopedActingPlayerContext context(*player, actor);
        stat_recalc_derived(actor);
    }
    return true;
}

static bool runCharacterAdvancementSmoke()
{
    bool passed = true;
    for (PlayerId id : session.players().playerIds()) {
        Object* actor = networkWorldPlayerActor(id);
        auto* player = session.players().find(id);
        if (actor == nullptr || player == nullptr) return false;
        auto saved = player->build;
        int hp = actor->data.critter.hp;
        ScopedActingPlayerContext context(*player, actor);
        CharacterBuild baseline;
        baseline.baseStats = saved.baseStats;
        baseline.baseStats[STAT_PERCEPTION] = 6;
        baseline.baseStats[STAT_INTELLIGENCE] = 6;
        baseline.baseStats[STAT_ENDURANCE] = 6;
        baseline.level = baseline.processedLevel = 12;
        baseline.pendingPerks = 1;
        baseline.unspentSkillPoints = 10;
        player->build = baseline;
        stat_recalc_derived(actor);
        baseline = player->build;
        CharacterAdvanceCommand command;
        command.expectedBuild = characterAdvancementFingerprint(baseline);
        command.perk = PERK_AWARENESS;
        command.skillIncrements[SKILL_FIRST_AID] = 3;
        auto reject = [&](CharacterAdvanceCommand invalid) {
            bool rejected = commandExecutor.advanceCharacter(actor, invalid) == CommandExecutionStatus::InvalidAction;
            return rejected && player->build == baseline && actor->data.critter.hp == hp;
        };
        auto invalid = command;
        ++invalid.expectedBuild;
        passed = reject(invalid) && passed;
        invalid = command;
        invalid.skillIncrements[SKILL_FIRST_AID] = 99;
        passed = reject(invalid) && passed;
        invalid = command;
        invalid.perk = PERK_SLAYER;
        passed = reject(invalid) && passed;
        bool committed = commandExecutor.advanceCharacter(actor, command) == CommandExecutionStatus::Applied;
        passed = committed && player->build.skillPoints[SKILL_FIRST_AID] == 3
            && player->build.unspentSkillPoints == 7 && player->build.perkRanks[PERK_AWARENESS] == 1
            && player->build.pendingPerks == 0 && passed;
        auto after = player->build;
        passed = commandExecutor.advanceCharacter(actor, command) == CommandExecutionStatus::InvalidAction
            && player->build == after && passed;
        player->build = baseline;
        command.skillIncrements.fill(0);
        command.perk = PERK_TAG;
        command.taggedSkill = SKILL_REPAIR;
        passed = commandExecutor.advanceCharacter(actor, command) == CommandExecutionStatus::Applied
            && player->build.taggedSkills[3] == SKILL_REPAIR && player->build.pendingPerks == 0 && passed;
        player->build = baseline;
        player->build.traits = { TRAIT_GIFTED, TRAIT_SKILLED };
        command.expectedBuild = characterAdvancementFingerprint(player->build);
        command.perk = PERK_MUTATE;
        command.taggedSkill = -1;
        command.removedTrait = TRAIT_SKILLED;
        command.addedTrait = TRAIT_FAST_SHOT;
        passed = commandExecutor.advanceCharacter(actor, command) == CommandExecutionStatus::Applied
            && player->build.traits[0] == TRAIT_GIFTED && player->build.traits[1] == TRAIT_FAST_SHOT
            && player->build.pendingPerks == 0 && passed;
        player->build = saved;
        actor->data.critter.hp = hp;
    }
    std::fprintf(stderr, "NATIVE_CHARACTER_ADVANCEMENT_%s actors=%zu stale=1 overdraw=1 prerequisites=1 skills=1 perks=1 tag=1 mutate=1\n",
        passed ? "PASS" : "FAIL", session.players().size());
    return passed;
}

bool networkWorldPrepareAutomapSmoke()
{
    if (worldMode != NetworkLaunchMode::Host) return false;
    int floor = 0;
    for (PlayerId id : session.players().playerIds()) {
        Object* actor = networkWorldPlayerActor(id);
        Object* sensor = nullptr;
        if (actor == nullptr || obj_move_to_tile(actor, actor->tile, floor++ % ELEVATION_COUNT, nullptr) != 0
            || obj_pid_new(&sensor, PROTO_ID_MOTION_SENSOR) != 0 || sensor == nullptr) return false;
        obj_disconnect(sensor, nullptr);
        item_m_set_charges(sensor, 2);
        if (item_add_force(actor, sensor, 1) != 0 || !registerItem(sensor)) return false;
        if (Object* held = inven_right_hand(actor)) held->flags &= ~OBJECT_IN_RIGHT_HAND;
        sensor->flags |= OBJECT_IN_RIGHT_HAND;
    }
    return true;
}

static bool runAutomapScannerAuthoritySmoke()
{
    bool passed = true;
    std::size_t eventStart = deferredEvents.size();
    for (PlayerId id : session.players().playerIds()) {
        Object* actor = networkWorldPlayerActor(id);
        auto* player = playerStateForActor(actor);
        if (actor == nullptr || player == nullptr) return false;
        ScopedActingPlayerContext context(*player, actor);
        ScopedLocalPlayerBinding binding(actor);
        std::vector<std::pair<Object*, int>> flags;
        for (int i = 0; i < actor->data.inventory.length; ++i) {
            Object* item = actor->data.inventory.items[i].item;
            flags.emplace_back(item, item->flags);
            item->flags &= ~OBJECT_IN_RIGHT_HAND;
        }
        Object* sensor = nullptr;
        bool ready = obj_pid_new(&sensor, PROTO_ID_MOTION_SENSOR) == 0 && sensor != nullptr;
        if (ready) {
            obj_disconnect(sensor, nullptr);
            item_m_set_charges(sensor, 3);
            ready = item_add_force(actor, sensor, 2) == 0 && registerItem(sensor);
        }
        bool tested = false;
        if (ready) {
            sensor->flags |= OBJECT_IN_RIGHT_HAND;
            EntityId sensorId = networkWorldFindEntity(sensor).value_or(EntityId {});
            int ap = actor->data.critter.combat.ap;
            CommandProcessor processor;
            GameCommand command { CommandSequence { 1 }, id, player->actorId, SessionPhase::Exploration,
                session.phaseRevision(), InventoryActionCommand { 0, sensorId, InventoryAction::Scan } };
            auto applied = processor.process(command, session, commandExecutor);
            bool charged = applied.result.status == CommandStatus::Accepted
                && item_m_curr_charges(sensor) == 2 && item_count(actor, sensor) == 1
                && inven_right_hand(actor) == sensor && actor->data.critter.combat.ap == ap;
            int sensors = 0, totalCharges = 0, equipped = 0;
            for (int i = 0; i < actor->data.inventory.length; ++i) {
                const auto& entry = actor->data.inventory.items[i];
                if (entry.item->pid != PROTO_ID_MOTION_SENSOR) continue;
                sensors += entry.quantity; totalCharges += entry.quantity * item_m_curr_charges(entry.item);
                equipped += (entry.item->flags & OBJECT_IN_RIGHT_HAND) != 0;
            }
            bool stack = sensors == 2 && totalCharges == 5 && equipped == 1;
            auto replay = processor.process(command, session, commandExecutor);
            bool replaySafe = replay.replayed && item_m_curr_charges(sensor) == 2;
            command.sequence.value++;
            sensor->flags &= ~OBJECT_IN_RIGHT_HAND;
            bool unequipped = processor.process(command, session, commandExecutor).result.status == CommandStatus::Rejected
                && item_m_curr_charges(sensor) == 2;
            sensor->flags |= OBJECT_IN_RIGHT_HAND;
            command.sequence.value++;
            item_m_set_charges(sensor, 0);
            bool empty = processor.process(command, session, commandExecutor).result.status == CommandStatus::Rejected
                && item_m_curr_charges(sensor) == 0;
            item_m_set_charges(sensor, 2);
            bool foreign = true, markers = automap_player_marker(actor, actor->elevation) == 1
                && automap_player_marker(actor, actor->elevation + 1) == 0;
            for (PlayerId otherId : session.players().playerIds()) {
                if (otherId == id) continue;
                Object* other = networkWorldPlayerActor(otherId);
                auto* otherPlayer = playerStateForActor(other);
                command.sequence.value = 1; command.playerId = otherId; command.actorId = otherPlayer->actorId;
                foreign = processor.process(command, session, commandExecutor).result.status == CommandStatus::Rejected
                    && item_m_curr_charges(sensor) == 2 && foreign;
                markers = automap_player_marker(other, other->elevation) == 2
                    && automap_player_marker(other, other->elevation + 1) == 0 && markers;
            }
            ItemDescriptor descriptor;
            bool snapshot = describeItem(sensor, descriptor);
            Object* copy = nullptr;
            snapshot = obj_pid_new(&copy, PROTO_ID_MOTION_SENSOR) == 0 && copy != nullptr && snapshot;
            if (copy != nullptr) {
                applyItemDescriptor(copy, descriptor);
                snapshot = item_m_curr_charges(copy) == 2 && snapshot;
                obj_erase_object(copy, nullptr);
            }
            tested = charged && stack && replaySafe && unequipped && empty && foreign && markers && snapshot;
            std::fprintf(stderr, "NATIVE_AUTOMAP_SCANNER_%s player=%u charge=%d stack=%d replay=%d unequipped=%d empty=%d foreign=%d markers=%d descriptor=%d\n",
                tested ? "PASS" : "FAIL", id.value, charged, stack, replaySafe, unequipped, empty, foreign, markers, snapshot);
        }
        // This fixture's motion sensors have no baseline counterparts.
        for (int i = actor->data.inventory.length - 1; i >= 0; --i) {
            Object* item = actor->data.inventory.items[i].item;
            if (item->pid == PROTO_ID_MOTION_SENSOR) {
                item_remove_mult(actor, item, item_count(actor, item)); obj_erase_object(item, nullptr);
            }
        }
        for (const auto& entry : flags) entry.first->flags = entry.second;
        passed = tested && passed;
    }
    while (deferredEvents.size() > eventStart) deferredEvents.pop_back();
    return passed;
}

bool networkWorldPrepareExplosiveTimerSmoke()
{
    if (worldMode != NetworkLaunchMode::Host || session.phase() != SessionPhase::Exploration) return false;
    for (PlayerId id : session.players().playerIds()) {
        Object* actor = networkWorldPlayerActor(id);
        auto* player = session.players().find(id);
        if (actor == nullptr || player == nullptr) return false;
        player->build.skillPoints[SKILL_TRAPS] = 100;
        player->build.baseStats[STAT_LUCK] = 10;
        Object* explosive = nullptr;
        if (obj_pid_new(&explosive, PROTO_ID_DYNAMITE_I) != 0 || explosive == nullptr
            || obj_disconnect(explosive, nullptr) != 0
            || item_add_force(actor, explosive, 2) != 0 || !registerItem(explosive)) return false;
    }
    return true;
}

static bool runExplosiveRollAndSourceSmoke()
{
    bool passed = true;
    int savedTime = game_time();
    set_game_time(std::max(savedTime, GAME_TIME_TICKS_PER_DAY * 2));
    for (PlayerId id : session.players().playerIds()) {
        Object* actor = networkWorldPlayerActor(id);
        auto* player = session.players().find(id);
        if (actor == nullptr || player == nullptr) { set_game_time(savedTime); return false; }
        auto savedBuild = player->build;
        ScopedActingPlayerContext context(*player, actor);
        player->build.skillPoints[SKILL_TRAPS] = 0;
        player->build.baseStats[STAT_PERCEPTION] = 2;
        player->build.baseStats[STAT_AGILITY] = 2;
        player->build.baseStats[STAT_LUCK] = 1;
        player->build.traits = { -1, -1 };
        player->build.perkRanks.fill(0);
        bool sawSuccess = false, sawFailure = false, sawCritical = false;
        // Native reseeding retains part of its shuffle table. Observe real
        // host rolls rather than predicting a second roll by reseeding.
        for (int attempt = 0; attempt < 300 && !(sawSuccess && sawFailure && sawCritical); ++attempt) {
            Object* bomb = nullptr;
            if (obj_pid_new(&bomb, PROTO_ID_PLASTIC_EXPLOSIVES_I) != 0 || bomb == nullptr) { passed = false; break; }
            obj_disconnect(bomb, nullptr);
            item_add_force(actor, bomb, 1);
            bool armed = obj_arm_explosive(actor, bomb, 120) == 0;
            std::vector<QueueEventState> events;
            bool captured = queue_capture_state(events);
            auto timer = std::find_if(events.begin(), events.end(), [&](const auto& event) { return event.owner == bomb; });
            bool countdown = captured && timer != events.end() && timer->payloadCount == 1
                && timer->payload[0] == static_cast<int>(id.value);
            int type = EVENT_TYPE_PLAYER_EXPLOSION;
            if (countdown) {
                type = timer->eventType;
                int delay = timer->time - game_time();
                if (type == EVENT_TYPE_PLAYER_EXPLOSION && delay == 1200) sawSuccess = true;
                else if (type == EVENT_TYPE_PLAYER_EXPLOSION_FAILURE && delay == 600) sawFailure = true;
                else if (type == EVENT_TYPE_PLAYER_EXPLOSION_FAILURE && delay == 0) sawCritical = true;
                else countdown = false;
            }
            passed = armed && countdown && passed;
            PlayerExplosionEvent data { static_cast<int>(id.value) };
            DB_FILE* file = db_fopen("explosive-event-smoke.bin", "wb");
            bool written = file != nullptr && q_func[type].writeProc(file, &data) == 0;
            if (file != nullptr) db_fclose(file);
            file = db_fopen("explosive-event-smoke.bin", "rb");
            void* loaded = nullptr;
            bool read = file != nullptr && q_func[type].readProc(file, &loaded) == 0;
            if (file != nullptr) db_fclose(file);
            passed = written && read && loaded != nullptr
                && static_cast<PlayerExplosionEvent*>(loaded)->playerId == static_cast<int>(id.value) && passed;
            if (loaded != nullptr) mem_free(loaded);
            queue_remove(bomb);
            if (bomb->owner != nullptr) item_remove_mult(bomb->owner, bomb, 1);
            obj_erase_object(bomb, nullptr);
        }
        passed = sawSuccess && sawFailure && sawCritical && passed;
        // Detonation follows the planted item's holder/location, while damage
        // attribution follows the original arming player after transfer/drop.
        for (bool planted : { false, true }) {
            Object* victim = nullptr;
            Object* bomb = nullptr;
            bool made = obj_pid_new(&victim, 0x100000B) == 0 && victim != nullptr
                && obj_move_to_tile(victim, 18900, 1, nullptr) == 0
                && obj_pid_new(&bomb, PROTO_ID_DYNAMITE_I) == 0 && bomb != nullptr;
            if (made) {
                victim->data.critter.hp = 200;
                obj_disconnect(bomb, nullptr);
                item_add_force(actor, bomb, 1);
                made = obj_arm_explosive(actor, bomb, 120) == 0
                    && item_remove_mult(actor, bomb, 1) == 0;
                made = made && (planted ? item_add_force(victim, bomb, 1) == 0
                    : obj_connect(bomb, victim->tile, victim->elevation, nullptr) == 0);
            }
            if (made) {
                PlayerExplosionEvent data { static_cast<int>(id.value) };
                q_func[EVENT_TYPE_PLAYER_EXPLOSION].field_14(bomb, &data);
                bomb = nullptr; // Native detonation destroys the item.
                passed = victim->data.critter.hp < 200
                    && victim->data.critter.combat.whoHitMe == actor && passed;
                Script requestOwner {};
                requestOwner.owner = victim;
                scripts_clear_combat_requests(&requestOwner);
            } else passed = false;
            if (bomb != nullptr) {
                queue_remove(bomb);
                if (bomb->owner != nullptr) item_remove_mult(bomb->owner, bomb, 1);
                obj_erase_object(bomb, nullptr);
            }
            if (victim != nullptr) obj_erase_object(victim, nullptr);
        }
        player->build = savedBuild;
    }
    set_game_time(savedTime);
    std::fprintf(stderr, "NATIVE_EXPLOSIVE_ROLL_SOURCE_%s success=1 failure_half_timer=1 critical_immediate=1 native_payload_save_load=1 dropped_source=1 planted_source=1\n", passed ? "PASS" : "FAIL");
    return passed;
}

static bool runExplosiveTimerAuthoritySmoke()
{
    if (session.phase() != SessionPhase::Exploration) return false;
    bool passed = true;
    auto feedbackStart = deferredEvents.size();
    for (PlayerId id : session.players().playerIds()) {
        auto* player = session.players().find(id);
        Object* actor = networkWorldPlayerActor(id);
        if (player == nullptr || actor == nullptr) return false;
        ScopedActingPlayerContext context(*player, actor);
        int ap = actor->data.critter.combat.ap;
        std::unordered_set<std::uint32_t> original;
        for (const auto& entry : worldItems) original.insert(entry.first.value);
        Object* explosive = nullptr;
        if (obj_pid_new(&explosive, PROTO_ID_DYNAMITE_I) != 0 || explosive == nullptr
            || obj_disconnect(explosive, nullptr) != 0
            || item_add_force(actor, explosive, 2) != 0 || !registerItem(explosive)) return false;
        EntityId itemId = networkWorldFindEntity(explosive).value_or(EntityId {});
        CommandProcessor processor;
        GameCommand command { CommandSequence { 1 }, player->id, player->actorId,
            SessionPhase::Exploration, session.phaseRevision(),
            InventoryActionCommand { 0, itemId, InventoryAction::Use, {}, 1, 0 } };
        passed = processor.process(command, session, commandExecutor).result.status == CommandStatus::Rejected
            && explosive->pid == PROTO_ID_DYNAMITE_I && item_count(actor, explosive) == 2 && passed;
        command.sequence.value++;
        std::get<InventoryActionCommand>(command.payload).timerSeconds = 120;
        auto result = processor.process(command, session, commandExecutor);
        passed = result.result.status == CommandStatus::Accepted && explosive->pid == PROTO_ID_DYNAMITE_II
            && (explosive->flags & OBJECT_USED) != 0 && item_count(actor, explosive) == 1 && passed;
        int unarmed = 0;
        for (const auto& entry : worldItems) {
            if (original.count(entry.first.value) == 0 && entry.second->owner == actor
                && entry.second->pid == PROTO_ID_DYNAMITE_I) unarmed += item_count(actor, entry.second);
        }
        std::vector<QueueEventState> timers;
        bool captured = queue_capture_state(timers);
        auto count = [&](const auto& values) {
            return std::count_if(values.begin(), values.end(), [&](const auto& event) {
                return event.owner == explosive && (event.eventType == EVENT_TYPE_PLAYER_EXPLOSION
                    || event.eventType == EVENT_TYPE_PLAYER_EXPLOSION_FAILURE);
            });
        };
        passed = captured && unarmed == 1 && count(timers) == 1
            && processor.process(command, session, commandExecutor).replayed && passed;
        command.sequence.value++;
        passed = processor.process(command, session, commandExecutor).result.status == CommandStatus::Rejected
            && actor->data.critter.combat.ap == ap && passed;
        command.sequence.value++;
        command.playerId = player->id == kHostPlayerId ? kGuestPlayerId : kHostPlayerId;
        command.actorId = session.playerActorId(command.playerId);
        passed = processor.process(command, session, commandExecutor).result.status == CommandStatus::Rejected && passed;
        std::vector<QueueEventState> after;
        passed = queue_capture_state(after) && count(after) == 1 && passed;
        // Restore the native queue from its serialized representation without
        // losing the armed item's object binding or adding another timer.
        passed = queue_replace_state(after) && queue_find(explosive,
            queue_find(explosive, EVENT_TYPE_PLAYER_EXPLOSION) ? EVENT_TYPE_PLAYER_EXPLOSION : EVENT_TYPE_PLAYER_EXPLOSION_FAILURE) && passed;
        std::vector<Object*> cleanup;
        for (const auto& entry : worldItems) if (original.count(entry.first.value) == 0) cleanup.push_back(entry.second);
        for (Object* item : cleanup) {
            queue_remove(item);
            if (item->owner != nullptr) item_remove_mult(item->owner, item, item_count(item->owner, item));
            obj_erase_object(item, nullptr);
        }
    }
    passed = runExplosiveRollAndSourceSmoke() && passed;
    while (deferredEvents.size() > feedbackStart) deferredEvents.pop_back();
    std::fprintf(stderr, "NATIVE_EXPLOSIVE_TIMER_%s actors=%zu missing_choice=1 split=1 replay=1 rearm_rejected=1 foreign_rejected=1 ap_unchanged=1 queue_recovery=1\n",
        passed ? "PASS" : "FAIL", session.players().size());
    return passed;
}

std::optional<WorldDiscoverySmokeFixture> networkWorldVerifyWorldDiscoverySmokeTest()
{
    WorldDiscoverySmokeFixture fixture;
    Object* ground = nullptr;
    Object* npc = nullptr;
    for (const auto& entry : worldItems) {
        Object* item = entry.second;
        if (item->pid == 211 && item->tile == 38000 && item->elevation == 0 && item->owner == nullptr) {
            fixture.groundId = entry.first;
            ground = item;
        }
    }
    if (ground == nullptr) return std::nullopt;
    for (int index = 0; index < ground->data.inventory.length; ++index) {
        const auto& entry = ground->data.inventory.items[index];
        if (entry.item->pid == kAntidotePid && entry.quantity == 3) {
            fixture.childId = session.entities().findEntity(entry.item).value_or(EntityId {});
        }
    }
    for (const auto& entry : worldCritters) {
        if (entry.second->pid == 0x0100000B && entry.second->tile == 38002 && entry.second->elevation == 0) {
            fixture.npcId = entry.first;
            npc = entry.second;
        }
    }
    for (const auto& entry : worldScenery) {
        if (entry.second->pid == 33554499 && entry.second->tile == 38004 && entry.second->elevation == 0) {
            fixture.sceneryId = entry.first;
        }
    }
    if (!isValid(fixture.childId) || !isValid(fixture.npcId) || !isValid(fixture.sceneryId)) return std::nullopt;
    std::vector<QueueEventState> timers;
    if (!queue_capture_state(timers)) return std::nullopt;
    for (const auto& timer : timers) {
        if (timer.owner == npc && timer.eventType == EVENT_TYPE_KNOCKOUT) fixture.timerTime = timer.time;
    }
    return fixture.timerTime > game_time() ? std::optional<WorldDiscoverySmokeFixture>(fixture) : std::nullopt;
}

std::optional<WorldDiscoverySmokeFixture> networkWorldPrepareWorldDiscoverySmokeTest()
{
    if (worldMode != NetworkLaunchMode::Host || !session.isActive()
        || networkWorldVerifyWorldDiscoverySmokeTest().has_value()) return std::nullopt;
    Object* ground = nullptr;
    Object* child = nullptr;
    Object* npc = nullptr;
    Object* scenery = nullptr;
    bool keep = false;
    struct Cleanup {
        Object*& ground;
        Object*& npc;
        Object*& scenery;
        bool& keep;
        ~Cleanup() {
            if (keep) return;
            for (Object* object : { ground, npc, scenery }) {
                if (object != nullptr) {
                    queue_remove(object);
                    obj_erase_object(object, nullptr);
                }
            }
        }
    } cleanup { ground, npc, scenery, keep };
    if (obj_pid_new(&ground, 211) != 0 || ground == nullptr
        || obj_pid_new(&child, kAntidotePid) != 0 || child == nullptr) return std::nullopt;
    if (item_add_force(ground, child, 3) != 0) {
        obj_erase_object(child, nullptr);
        return std::nullopt;
    }
    if (obj_disconnect(child, nullptr) != 0) return std::nullopt;
    if (obj_move_to_tile(ground, 38000, 0, nullptr) != 0
        || obj_pid_new(&npc, 0x0100000B) != 0 || npc == nullptr
        || obj_move_to_tile(npc, 38002, 0, nullptr) != 0
        || obj_pid_new(&scenery, 33554499) != 0 || scenery == nullptr
        || obj_move_to_tile(scenery, 38004, 0, nullptr) != 0) return std::nullopt;
    for (Object* object : { ground, npc, scenery }) {
        object->flags = (object->flags | OBJECT_NO_SAVE) & ~OBJECT_NO_REMOVE;
        if (object->sid != -1) {
            scr_remove(object->sid);
            object->sid = -1;
        }
    }
    if (session.entities().findEntity(ground).has_value()
        || session.entities().findEntity(child).has_value()
        || session.entities().findEntity(npc).has_value()
        || session.entities().findEntity(scenery).has_value()
        || queue_add(1000000, npc, nullptr, EVENT_TYPE_KNOCKOUT) != 0) return std::nullopt;
    WorldSnapshot snapshot;
    if (!networkWorldCaptureSnapshot({}, snapshot)) return std::nullopt;
    auto verified = networkWorldVerifyWorldDiscoverySmokeTest();
    keep = verified.has_value();
    return verified;
}

bool networkWorldEraseWorldDiscoverySmokeTest()
{
    if (worldMode != NetworkLaunchMode::Join) return false;
    auto fixture = networkWorldVerifyWorldDiscoverySmokeTest();
    if (!fixture.has_value()) return false;
    for (EntityId id : { fixture->groundId, fixture->npcId, fixture->sceneryId }) {
        Object* object = session.entities().findObject(id);
        if (object == nullptr) return false;
        queue_remove(object);
        object->flags &= ~OBJECT_NO_REMOVE;
        if (obj_erase_object(object, nullptr) != 0) return false;
    }
    return !networkWorldVerifyWorldDiscoverySmokeTest().has_value()
        && session.entities().findObject(fixture->groundId) == nullptr
        && session.entities().findObject(fixture->childId) == nullptr
        && session.entities().findObject(fixture->npcId) == nullptr
        && session.entities().findObject(fixture->sceneryId) == nullptr;
}

static bool runScriptCreatedWorldObjectSmokeTest()
{
    if (worldMode != NetworkLaunchMode::Host || worldScenery.empty()) return false;
    Object* host = networkWorldPlayerActor(kHostPlayerId);
    if (host == nullptr) return false;
    Object* ground = nullptr;
    Object* child = nullptr;
    Object* npc = nullptr;
    Object* scenery = nullptr;
    struct Cleanup {
        Object*& ground;
        Object*& npc;
        Object*& scenery;
        ~Cleanup()
        {
            for (Object* object : { ground, npc, scenery }) {
                if (object != nullptr) {
                    queue_remove(object);
                    obj_erase_object(object, nullptr);
                }
            }
        }
    } cleanup { ground, npc, scenery };
    if (obj_pid_new(&ground, 211) != 0 || ground == nullptr
        || obj_pid_new(&child, kAntidotePid) != 0 || child == nullptr) return false;
    if (item_add_force(ground, child, 3) != 0) {
        obj_erase_object(child, nullptr);
        return false;
    }
    if (obj_disconnect(child, nullptr) != 0) return false;
    if (obj_move_to_tile(ground, 0, host->elevation, nullptr) != 0
        || obj_pid_new(&npc, 0x0100000B) != 0 || npc == nullptr
        || obj_move_to_tile(npc, 0, host->elevation, nullptr) != 0
        || obj_pid_new(&scenery, worldScenery.front().second->pid) != 0 || scenery == nullptr
        || obj_move_to_tile(scenery, 0, host->elevation, nullptr) != 0) return false;
    // Tile zero makes the new objects part of the iterator's repeated first
    // tile, exercising discovery deduplication before the wire validation.
    // This matches a native create_object call with no script attached. Keep
    // destruction cleanup from running unrelated prototype scripts.
    for (Object* object : { ground, npc, scenery }) {
        if (object->sid != -1) {
            scr_remove(object->sid);
            object->sid = -1;
        }
    }
    bool initiallyUntracked = !session.entities().findEntity(ground).has_value()
        && !session.entities().findEntity(child).has_value()
        && !session.entities().findEntity(npc).has_value()
        && !session.entities().findEntity(scenery).has_value();
    if (!initiallyUntracked || queue_add(100, npc, nullptr, EVENT_TYPE_KNOCKOUT) != 0) return false;
    auto phase = session.phase();
    auto revision = session.phaseRevision();
    auto roster = playerActorRoster();
    auto combatRevision = combatTurns.revision();
    WorldSnapshot snapshot;
    bool captured = networkWorldCaptureSnapshot({}, snapshot);
    auto groundId = session.entities().findEntity(ground);
    auto childId = session.entities().findEntity(child);
    auto npcId = session.entities().findEntity(npc);
    auto sceneryId = session.entities().findEntity(scenery);
    auto hasItem = [&](std::optional<EntityId> id, EntityId holder, std::uint32_t quantity) {
        return id.has_value() && std::any_of(snapshot.items.begin(), snapshot.items.end(),
            [&](const ItemSnapshot& item) {
                return item.entityId == *id && item.holderId == holder && item.quantity == quantity;
            });
    };
    bool sections = captured && groundId.has_value() && childId.has_value()
        && npcId.has_value() && sceneryId.has_value()
        && hasItem(groundId, {}, 1) && hasItem(childId, *groundId, 3)
        && std::any_of(snapshot.critters.begin(), snapshot.critters.end(),
            [&](const CritterSnapshot& critter) { return critter.entityId == *npcId; })
        && std::any_of(snapshot.scenery.begin(), snapshot.scenery.end(),
            [&](const ScenerySnapshot& state) { return state.entityId == *sceneryId; })
        && std::any_of(snapshot.timedEvents.begin(), snapshot.timedEvents.end(),
            [&](const TimedEventSnapshot& timer) {
                return timer.ownerId == *npcId && timer.eventType == EVENT_TYPE_KNOCKOUT;
            });
    Packet packet;
    bool wire = sections && encodeSnapshot(snapshot, packet) == SnapshotError::None && decodeSnapshot(packet);
    auto afterRoster = playerActorRoster();
    bool preserved = session.phase() == phase && session.phaseRevision() == revision
        && combatTurns.revision() == combatRevision && roster.has_value() && afterRoster.has_value()
        && roster->size() == afterRoster->size();
    for (std::size_t index = 0; preserved && index < roster->size(); ++index) {
        preserved = (*roster)[index].playerId == (*afterRoster)[index].playerId
            && (*roster)[index].actorId == (*afterRoster)[index].actorId
            && (*roster)[index].actor == (*afterRoster)[index].actor;
    }
    std::fprintf(stderr,
        "NATIVE_SCRIPT_CREATED_WORLD_OBJECTS_%s untracked=%d captured=%d sections=%d wire=%d phase_roster_preserved=%d timer_owner=%u.\n",
        sections && wire && preserved ? "PASS" : "FAIL", initiallyUntracked ? 1 : 0,
        captured ? 1 : 0, sections ? 1 : 0, wire ? 1 : 0, preserved ? 1 : 0,
        npcId.value_or(EntityId {}).value);
    return sections && wire && preserved;
}

bool networkWorldRunEngineAuthoritySmokeTest(EngineExecutionProbeCounts& counts)
{
    counts = {};
    if (worldMode == NetworkLaunchMode::Host && (!runCharacterAdvancementSmoke() || !runExplosiveTimerAuthoritySmoke() || !runAutomapScannerAuthoritySmoke())) return false;
    if (!session.isActive()) {
        std::fprintf(stderr, "Multiplayer authority probe: inactive world.\n");
        return false;
    }

    Object* scriptedDoor = nullptr;
    EntityId scriptedDoorId;
    for (const auto& entry : worldDoors) {
        int sid = -1;
        if (obj_sid(entry.second, &sid) != -1) {
            scriptedDoor = entry.second;
            scriptedDoorId = entry.first;
            break;
        }
    }
    if (scriptedDoor == nullptr) {
        std::fprintf(stderr, "Multiplayer authority probe: no scripted door.\n");
        return false;
    }

    Object* hostActor = networkWorldPlayerActor(kHostPlayerId);
    Object* target = worldCritters.empty() ? nullptr : worldCritters.front().second;
    EntityId hostActorId;
    EntityId targetId;
    if (hostActor == nullptr || target == nullptr) {
        std::fprintf(stderr, "Multiplayer authority probe: missing host or non-player actor.\n");
        return false;
    }
    std::optional<EntityId> registeredHostActor = session.entities().findEntity(hostActor);
    std::optional<EntityId> registeredTarget = target != nullptr
        ? session.entities().findEntity(target)
        : std::nullopt;
    if (!registeredHostActor.has_value() || !registeredTarget.has_value()) {
        std::fprintf(stderr, "Multiplayer authority probe: actor is not registered.\n");
        return false;
    }
    hostActorId = *registeredHostActor;
    targetId = *registeredTarget;

    if (worldMode == NetworkLaunchMode::Host) {
        if (!runScriptCreatedWorldObjectSmokeTest()) return false;
        if (!runTheftScriptAndSneakSmoke() || !runTheftTransactionSmokeTest() || !runNpcBarterSmokeTest()) return false;
        Object* corpse = nullptr;
        if (obj_pid_new(&corpse, 0x100000B) != 0 || corpse == nullptr) return false;
        corpse->flags |= OBJECT_MULTIHEX;
        corpse->data.critter.combat.results |= DAM_DEAD;
        bool overlap = obj_move_to_tile(corpse, hostActor->tile, hostActor->elevation, nullptr) == 0
            && obj_dist(hostActor, corpse) == 0 && lootTargetIsInRange(hostActor, corpse);
        int adjacentTile = tile_num_in_direction(hostActor->tile, ROTATION_SE, 2);
        bool adjacent = obj_move_to_tile(corpse, adjacentTile, hostActor->elevation, nullptr) == 0
            && obj_dist(hostActor, corpse) == 1 && lootTargetIsInRange(hostActor, corpse);
        int distantTile = tile_num_in_direction(hostActor->tile, ROTATION_SE, 5);
        bool distantRejected = obj_move_to_tile(corpse, distantTile, hostActor->elevation, nullptr) == 0
            && !lootTargetIsInRange(hostActor, corpse);
        Object* guestActor = networkWorldPlayerActor(kGuestPlayerId);
        Object* testAntidote = nullptr;
        if (guestActor == nullptr || obj_pid_new(&testAntidote, kAntidotePid) == -1
            || testAntidote == nullptr) {
            obj_erase_object(corpse, nullptr);
            return false;
        }
        std::size_t feedbackStart = deferredEvents.size();
        int failedUse = protinst_use_item_on(guestActor, corpse, testAntidote);
        const auto* itemFeedback = deferredEvents.size() == feedbackStart + 1
            ? std::get_if<PlayerFeedbackEvent>(&deferredEvents.back().payload) : nullptr;
        bool failedUseRouted = failedUse == -1 && itemFeedback != nullptr
            && itemFeedback->actorId == session.playerActorId(kGuestPlayerId)
            && !itemFeedback->text.empty();
        Object* testFlare = nullptr;
        bool flareCreated = obj_pid_new(&testFlare, PROTO_ID_FLARE) == 0 && testFlare != nullptr
            && item_add_force(corpse, testFlare, 2) == 0;
        std::size_t flareFeedbackStart = deferredEvents.size();
        int flareUse = flareCreated ? protinst_use_item(guestActor, testFlare) : -1;
        const auto* flareFeedback = deferredEvents.size() == flareFeedbackStart + 1
            ? std::get_if<PlayerFeedbackEvent>(&deferredEvents.back().payload) : nullptr;
        bool flareUseRouted = flareUse == 0 && flareFeedback != nullptr
            && flareFeedback->actorId == session.playerActorId(kGuestPlayerId)
            && !flareFeedback->text.empty();
        int unlitFlares = 0;
        int litFlares = 0;
        for (int index = 0; index < corpse->data.inventory.length; ++index) {
            const auto& entry = corpse->data.inventory.items[index];
            if (entry.item->pid == PROTO_ID_FLARE) unlitFlares += entry.quantity;
            if (entry.item->pid == PROTO_ID_LIT_FLARE) litFlares += entry.quantity;
        }
        bool flareStackIsolated = flareUse == 0 && unlitFlares == 1 && litFlares == 1
            && queue_find(testFlare, EVENT_TYPE_FLARE);
        std::fprintf(stderr, "NATIVE_FLARE_STACK_USE_%s unlit=%d lit=%d timer=%d\n",
            flareStackIsolated ? "PASS" : "FAIL", unlitFlares, litFlares,
            testFlare != nullptr && queue_find(testFlare, EVENT_TYPE_FLARE));
        if (testFlare != nullptr) obj_destroy(testFlare);
        bool boundGuestRouted = false;
        bool nestedHostKeptLocal = false;
        bool guestScopeRestored = false;
        {
            ScopedLocalPlayerBinding binding(guestActor);
            ScopedPlayerFeedback guestFeedback(guestActor);
            boundGuestRouted = networkWorldRoutePlayerFeedback("Guest quest result");
            {
                ScopedPlayerFeedback hostFeedback(hostActor);
                nestedHostKeptLocal = !networkWorldRoutePlayerFeedback("Host action result");
            }
            guestScopeRestored = networkWorldRoutePlayerFeedback("Guest quest continuation");
        }
        bool outsideScopeKeptLocal = !networkWorldRoutePlayerFeedback("Unrelated world result");
        while (deferredEvents.size() > feedbackStart) deferredEvents.pop_back();
        obj_erase_object(testAntidote, nullptr);
        obj_erase_object(corpse, nullptr);
        bool feedbackPassed = failedUseRouted && flareUseRouted && flareStackIsolated && boundGuestRouted
            && nestedHostKeptLocal && guestScopeRestored && outsideScopeKeptLocal;
        std::fprintf(stderr, "NATIVE_PLAYER_FEEDBACK_%s failed_item_use=%d flare_use=%d guest_binding=%d nested_host=%d scope_restored=%d unscoped_local=%d\n",
            feedbackPassed ? "PASS" : "FAIL", failedUseRouted, flareUseRouted, boundGuestRouted,
            nestedHostKeptLocal, guestScopeRestored, outsideScopeKeptLocal);
        if (!feedbackPassed) return false;
        std::fprintf(stderr, "NATIVE_MULTIHEX_LOOT_RANGE_%s overlap=%d adjacent=%d distant_rejected=%d\n",
            overlap && adjacent && distantRejected ? "PASS" : "FAIL", overlap, adjacent, distantRejected);
        if (!overlap || !adjacent || !distantRejected) return false;
        Object* reward = nullptr;
        bool rewardCreated = obj_pid_new(&reward, kAntidotePid) == 0
            && reward != nullptr && item_add_force(guestActor, reward, 1) == 0;
        bool initiallyUntracked = rewardCreated
            && !session.entities().findEntity(reward).has_value();
        WorldSnapshot rewardCheckpoint;
        bool capturedReward = initiallyUntracked
            && networkWorldCaptureSnapshot({}, rewardCheckpoint);
        // A preceding skill can leave a door animation running. Capture must
        // refuse that unstable state, just as it does during normal play;
        // let the native animation finish rather than clearing it or treating
        // a deferred checkpoint as a missing quest reward.
        auto rewardDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (initiallyUntracked && !capturedReward
            && std::chrono::steady_clock::now() < rewardDeadline) {
            process_bk();
            capturedReward = networkWorldCaptureSnapshot({}, rewardCheckpoint);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto rewardId = rewardCreated ? session.entities().findEntity(reward) : std::nullopt;
        bool rewardDelivered = capturedReward && rewardId.has_value()
            && std::any_of(rewardCheckpoint.items.begin(), rewardCheckpoint.items.end(),
                [&](const ItemSnapshot& item) {
                    return item.entityId == *rewardId
                        && item.holderId == session.playerActorId(kGuestPlayerId)
                        && item.itemDescriptor.pid == kAntidotePid && item.quantity == 1;
                });
        if (reward != nullptr) obj_destroy(reward);
        std::fprintf(stdout, "NATIVE_SCRIPT_REWARD_CHECKPOINT_%s initially_untracked=%d guest_delivery=%d\n",
            rewardDelivered ? "PASS" : "FAIL", initiallyUntracked, rewardDelivered);
        if (!rewardDelivered) return false;
        // Native scripts may put rewards on scenery or doors, with nested
        // containers. They must enter the same complete checkpoint as actor
        // inventory, without requiring a player to open that owner first.
        bool staticRewardsPassed = true;
        for (const auto* owners : { &worldDoors, &worldScenery }) {
            auto ownerEntry = std::find_if(owners->begin(), owners->end(),
                [](const auto& entry) { return entry.second->data.inventory.length == 0; });
            if (ownerEntry == owners->end()) {
                std::fprintf(stderr, "NATIVE_STATIC_SCRIPT_REWARD_FIXTURE_MISSING type=%s owners=%zu\n",
                    owners == &worldDoors ? "door" : "scenery", owners->size());
                staticRewardsPassed = false;
                break;
            }
            auto ownerState = *ownerEntry;
            Object* bag = nullptr;
            Object* child = nullptr;
            bool created = obj_pid_new(&bag, 211) == 0 && bag != nullptr
                && obj_disconnect(bag, nullptr) == 0
                && item_add_force(ownerState.second, bag, 1) == 0
                && obj_pid_new(&child, kAntidotePid) == 0 && child != nullptr
                && obj_disconnect(child, nullptr) == 0 && item_add_force(bag, child, 3) == 0;
            bool untracked = created && !session.entities().findEntity(bag).has_value()
                && !session.entities().findEntity(child).has_value();
            WorldSnapshot checkpoint;
            bool captured = untracked && networkWorldCaptureSnapshot({}, checkpoint);
            auto bagId = session.entities().findEntity(bag);
            auto childId = session.entities().findEntity(child);
            auto contains = [&](EntityId id, EntityId holder, std::uint32_t quantity) {
                return std::any_of(checkpoint.items.begin(), checkpoint.items.end(),
                    [&](const ItemSnapshot& item) {
                        return item.entityId == id && item.holderId == holder && item.quantity == quantity;
                    });
            };
            std::vector<std::uint8_t> encoded;
            bool delivered = captured && bagId.has_value() && childId.has_value()
                && contains(*bagId, ownerState.first, 1) && contains(*childId, *bagId, 3)
                && encodeSnapshot(checkpoint, encoded) == SnapshotError::None && decodeSnapshot(encoded);
            bool indexed = delivered && registerWorldObjects()
                && std::any_of(worldItems.begin(), worldItems.end(),
                    [&](const auto& entry) { return entry.first == *bagId && entry.second == bag; })
                && std::any_of(worldItems.begin(), worldItems.end(),
                    [&](const auto& entry) { return entry.first == *childId && entry.second == child; });
            bool retained = false;
            if (delivered) {
                Object* owner = ownerState.second;
                int originalFlags = owner->flags;
                owner->flags |= OBJECT_NO_REMOVE;
                retained = obj_erase_object(owner, nullptr) == -1
                    && session.entities().findObject(ownerState.first) == owner
                    && session.entities().findObject(*bagId) == bag
                    && session.entities().findObject(*childId) == child;
                owner->flags = originalFlags;
            }
            std::fprintf(stderr, "NATIVE_STATIC_SCRIPT_REWARD_OWNER type=%s created=%d untracked=%d captured=%d bag=%u child=%u delivered=%d\n",
                owners == &worldDoors ? "door" : "scenery", created, untracked, captured,
                bagId.value_or(EntityId {}).value, childId.value_or(EntityId {}).value, delivered);
            if (child != nullptr && child->owner != bag) obj_destroy(child);
            if (bag != nullptr) obj_destroy(bag);
            bool removed = bagId.has_value() && childId.has_value()
                && session.entities().findObject(*bagId) == nullptr
                && session.entities().findObject(*childId) == nullptr;
            std::fprintf(stderr, "NATIVE_STATIC_SCRIPT_REWARD_LIFETIME type=%s indexed=%d retained=%d removed_nested=%d\n",
                owners == &worldDoors ? "door" : "scenery", indexed, retained, removed);
            staticRewardsPassed = staticRewardsPassed && delivered && indexed && retained && removed;
        }
        std::fprintf(stdout, "NATIVE_STATIC_SCRIPT_REWARD_CHECKPOINT_%s doors=1 scenery=1 nested_quantity=3\n",
            staticRewardsPassed ? "PASS" : "FAIL");
        if (!staticRewardsPassed) return false;
        engineExecutionProbeBegin();
        int doorRc = obj_use_door(hostActor, scriptedDoor, 0);
        EngineExecutionProbeCounts doorCounts = engineExecutionProbeEnd();
        register_clear(scriptedDoor);
        if (doorRc == -1
            || doorCounts.scriptProcedures != 1
            || doorCounts.combatAttacks != 0) {
            std::fprintf(stderr, "Multiplayer authority probe: host door rc=%d scripts=%u attacks=%u rng=%u.\n",
                doorRc,
                doorCounts.scriptProcedures,
                doorCounts.combatAttacks,
                doorCounts.randomDraws);
            return false;
        }

        bool placed = false;
        for (int rotation = 0; rotation < ROTATION_COUNT && !placed; rotation++) {
            int tile = tile_num_in_direction(hostActor->tile, rotation, 1);
            if (obj_blocking_at(target, tile, hostActor->elevation) == nullptr) {
                placed = obj_move_to_tile(target, tile, hostActor->elevation, nullptr) == 0;
            }
        }
        if (!placed || obj_dist(hostActor, target) != 1) {
            std::fprintf(stderr, "Multiplayer authority probe: could not place combat target.\n");
            return false;
        }
        hostActor->data.critter.combat.ap = 10;
        engineExecutionProbeBegin();
        int attackRc = combat_attack(hostActor, target, HIT_MODE_PUNCH, HIT_LOCATION_TORSO);
        EngineExecutionProbeCounts attackCounts = engineExecutionProbeEnd();
        register_clear(hostActor);
        register_clear(target);
        if (attackRc == -1
            || attackCounts.scriptProcedures != 0
            || attackCounts.combatAttacks != 1
            || attackCounts.randomDraws == 0) {
            std::fprintf(stderr, "Multiplayer authority probe: host attack rc=%d scripts=%u attacks=%u rng=%u.\n",
                attackRc,
                attackCounts.scriptProcedures,
                attackCounts.combatAttacks,
                attackCounts.randomDraws);
            return false;
        }
        counts.scriptProcedures = doorCounts.scriptProcedures;
        counts.combatAttacks = attackCounts.combatAttacks;
        counts.randomDraws = attackCounts.randomDraws;
        return true;
    }

    if (worldMode != NetworkLaunchMode::Join) {
        std::fprintf(stderr, "Multiplayer authority probe: invalid world mode.\n");
        return false;
    }

    WorldSnapshot rejectionBaseline;
    if (!networkWorldCaptureSnapshot({}, rejectionBaseline)) return false;
    auto rejectionDigest = computeSnapshotDigest(rejectionBaseline);
    if (!rejectionDigest || rejectionBaseline.items.empty()) return false;
    auto rejectsWithoutMutation = [&](const WorldSnapshot& invalid, const char* reason) {
        std::vector<std::uint8_t> packet;
        if (encodeSnapshot(invalid, packet) != SnapshotError::None) return false;
        auto decoded = decodeSnapshot(packet);
        if (!decoded) return false;
        std::size_t registrySize = session.entities().size();
        engineExecutionProbeBegin();
        bool rejected = !networkWorldApplySnapshot(decoded.snapshot);
        WorldSnapshot after;
        bool captured = networkWorldCaptureSnapshot({}, after);
        EngineExecutionProbeCounts mutations = engineExecutionProbeEnd();
        auto afterDigest = computeSnapshotDigest(after);
        bool preserved = rejected && captured && afterDigest
            && rejectionDigest.digest.overall == afterDigest.digest.overall
            && registrySize == session.entities().size()
            && mutations.scriptProcedures == 0 && mutations.combatAttacks == 0
            && mutations.randomDraws == 0;
        std::fprintf(stderr,
            "NATIVE_SNAPSHOT_REJECTION_PRESERVES_STATE reason=%s rejected=%d preserved=%d.\n",
            reason, rejected ? 1 : 0, preserved ? 1 : 0);
        return preserved;
    };
    WorldSnapshot phaseMismatch = rejectionBaseline;
    phaseMismatch.phase = rejectionBaseline.phase == SessionPhase::Exploration
        ? SessionPhase::Transition : SessionPhase::Exploration;
    // Without entry preflight, this valid item section deletes every item
    // before the later phase check rejects the checkpoint.
    phaseMismatch.items.clear();
    phaseMismatch.timedEvents.clear();
    if (!rejectsWithoutMutation(phaseMismatch, "phase_mismatch")) return false;
    if (rejectionBaseline.phaseRevision > 1) {
        WorldSnapshot stale = rejectionBaseline;
        --stale.phaseRevision;
        stale.items.clear();
        stale.timedEvents.clear();
        if (!rejectsWithoutMutation(stale, "stale_phase")) return false;
    }
    WorldSnapshot wrongActor = rejectionBaseline;
    if (wrongActor.actors.size() < 2) return false;
    std::swap(wrongActor.actors[0].ownerId, wrongActor.actors[1].ownerId);
    wrongActor.mapId = rejectionBaseline.mapId == 0 ? 1 : 0;
    if (!rejectsWithoutMutation(wrongActor, "actor_before_map_load")) return false;
    if (rejectionBaseline.critters.empty() || rejectionBaseline.scenery.empty()) return false;
    WorldSnapshot unavailableCritter = rejectionBaseline;
    unavailableCritter.critters.back().pid = 0x010FFFFF;
    if (!rejectsWithoutMutation(unavailableCritter, "unavailable_critter_prototype")) return false;
    WorldSnapshot unavailableScenery = rejectionBaseline;
    unavailableScenery.scenery.back().pid = 0x020FFFFF;
    if (!rejectsWithoutMutation(unavailableScenery, "unavailable_scenery_prototype")) return false;
    WorldSnapshot unavailableItem = rejectionBaseline;
    unavailableItem.items.back().itemDescriptor.pid = 0x000FFFFF;
    if (!rejectsWithoutMutation(unavailableItem, "unavailable_item_prototype")) return false;
    if (rejectionBaseline.gameGlobalVariables.empty() || rejectionBaseline.doors.empty()) return false;
    WorldSnapshot incompatibleGlobals = rejectionBaseline;
    incompatibleGlobals.gameGlobalVariables.pop_back();
    incompatibleGlobals.mapLocalVariables.push_back(123456);
    if (!rejectsWithoutMutation(incompatibleGlobals, "globals_before_local_resize")) return false;
    // The same invariant also prevents replacing the current map on rejection.
    incompatibleGlobals.mapId = rejectionBaseline.mapId == 0 ? 1 : 0;
    if (!rejectsWithoutMutation(incompatibleGlobals, "globals_before_map_load")) return false;
    WorldSnapshot incompatibleMapGlobals = rejectionBaseline;
    if (incompatibleMapGlobals.mapGlobalVariables.empty()) {
        incompatibleMapGlobals.mapGlobalVariables.push_back(123456);
    } else {
        incompatibleMapGlobals.mapGlobalVariables.pop_back();
    }
    incompatibleMapGlobals.mapLocalVariables.push_back(123456);
    if (!rejectsWithoutMutation(incompatibleMapGlobals, "map_globals_before_local_resize")) return false;
    WorldSnapshot incompatibleDoors = rejectionBaseline;
    incompatibleDoors.doors.pop_back();
    incompatibleDoors.mapLocalVariables.push_back(123456);
    if (!rejectsWithoutMutation(incompatibleDoors, "doors_before_local_resize")) return false;
    WorldSnapshot expandedLocals = rejectionBaseline;
    expandedLocals.mapLocalVariables.push_back(123456);
    std::vector<std::uint8_t> expandedPacket;
    if (encodeSnapshot(expandedLocals, expandedPacket) != SnapshotError::None) return false;
    auto decodedExpanded = decodeSnapshot(expandedPacket);
    if (!decodedExpanded) return false;
    engineExecutionProbeBegin();
    bool expansionApplied = networkWorldApplySnapshot(decodedExpanded.snapshot);
    WorldSnapshot expandedCapture;
    bool expansionCaptured = networkWorldCaptureSnapshot({}, expandedCapture);
    bool localsExpanded = expansionApplied && expansionCaptured
        && expandedCapture.mapLocalVariables == expandedLocals.mapLocalVariables;
    bool expansionRestored = networkWorldApplySnapshot(rejectionBaseline);
    WorldSnapshot restoredCapture;
    bool restoredCaptured = networkWorldCaptureSnapshot({}, restoredCapture);
    EngineExecutionProbeCounts expansionCounts = engineExecutionProbeEnd();
    auto restoredDigest = computeSnapshotDigest(restoredCapture);
    bool localsRestored = expansionRestored && restoredCaptured && restoredDigest
        && restoredDigest.digest.overall == rejectionDigest.digest.overall
        && expansionCounts.scriptProcedures == 0 && expansionCounts.combatAttacks == 0
        && expansionCounts.randomDraws == 0;
    std::fprintf(stderr, "NATIVE_SNAPSHOT_LOCAL_VARIABLE_EXPANSION_PASS expanded=%d restored=%d.\n",
        localsExpanded ? 1 : 0, localsRestored ? 1 : 0);
    if (!localsExpanded || !localsRestored) return false;
    WorldSnapshot unavailableActorArt = rejectionBaseline;
    ++unavailableActorArt.actors.front().hitPoints;
    unavailableActorArt.actors.back().fid = (OBJ_TYPE_CRITTER << 24) | 0xFFF;
    ++unavailableActorArt.actors.back().frame;
    if (!rejectsWithoutMutation(unavailableActorArt, "unavailable_actor_art")) return false;
    WorldSnapshot unavailableActorFrame = rejectionBaseline;
    ++unavailableActorFrame.actors.front().hitPoints;
    unavailableActorFrame.actors.back().frame = 0x7FFFFFFF;
    if (!rejectsWithoutMutation(unavailableActorFrame, "actor_frame_out_of_bounds")) return false;
    WorldSnapshot unavailableCritterAlias = rejectionBaseline;
    unavailableCritterAlias.critters.back().fid = (OBJ_TYPE_CRITTER << 24)
        | (ANIM_ELECTRIFY << 16) | 0xFFF;
    if (!rejectsWithoutMutation(unavailableCritterAlias, "critter_alias_catalog_out_of_bounds")) return false;
    for (int animation : { ANIM_FALL_BACK, ANIM_BURNED_TO_NOTHING }) {
        WorldSnapshot nativePose = rejectionBaseline;
        ActorSnapshot& pose = nativePose.actors.back();
        pose.fid = art_id(OBJ_TYPE_CRITTER, pose.fid & 0xFFF, animation, 0, pose.rotation + 1);
        CacheEntry* handle = nullptr;
        Art* frames = art_ptr_lock(pose.fid, &handle);
        if (frames == nullptr) return false;
        pose.frame = art_frame_max_frame(frames) - 1;
        art_ptr_unlock(handle);
        pose.hitPoints = 0;
        pose.combatResults |= DAM_DEAD;
        std::vector<std::uint8_t> posePacket;
        if (encodeSnapshot(nativePose, posePacket) != SnapshotError::None) return false;
        auto decodedPose = decodeSnapshot(posePacket);
        if (!decodedPose) return false;
        engineExecutionProbeBegin();
        bool poseApplied = networkWorldApplySnapshot(decodedPose.snapshot);
        WorldSnapshot poseCapture;
        bool poseCaptured = networkWorldCaptureSnapshot({}, poseCapture);
        auto poseDigest = computeSnapshotDigest(nativePose);
        auto capturedPoseDigest = computeSnapshotDigest(poseCapture);
        bool poseMatched = poseApplied && poseCaptured && poseDigest && capturedPoseDigest
            && poseDigest.digest.overall == capturedPoseDigest.digest.overall;
        bool poseRestored = networkWorldApplySnapshot(rejectionBaseline);
        WorldSnapshot poseRestoreCapture;
        bool poseRestoreCaptured = networkWorldCaptureSnapshot({}, poseRestoreCapture);
        auto poseRestoreDigest = computeSnapshotDigest(poseRestoreCapture);
        EngineExecutionProbeCounts poseCounts = engineExecutionProbeEnd();
        bool poseRestoreMatched = poseRestored && poseRestoreCaptured && poseRestoreDigest
            && rejectionDigest.digest.overall == poseRestoreDigest.digest.overall
            && poseCounts.scriptProcedures == 0 && poseCounts.combatAttacks == 0
            && poseCounts.randomDraws == 0;
        std::fprintf(stderr,
            "NATIVE_SNAPSHOT_ACTOR_ART_CONTROL animation=%d applied=%d restored=%d.\n",
            animation, poseMatched ? 1 : 0, poseRestoreMatched ? 1 : 0);
        if (!poseMatched || !poseRestoreMatched) return false;
    }
    WorldSnapshot unavailableNpcFrame = rejectionBaseline;
    ++unavailableNpcFrame.actors.front().hitPoints;
    unavailableNpcFrame.critters.back().frame = 0x7FFFFFFF;
    if (!rejectsWithoutMutation(unavailableNpcFrame, "stable_npc_frame_out_of_bounds")) return false;
    std::uint32_t unusedCritterId = 1;
    while (session.entities().contains(EntityId { unusedCritterId })) ++unusedCritterId;
    WorldSnapshot rebasedNpcFrame = unavailableNpcFrame;
    if (rebasedNpcFrame.critters.size() < 2) return false;
    std::swap(rebasedNpcFrame.critters.front().entityId, rebasedNpcFrame.critters.back().entityId);
    // Native encounter populations can consist entirely of the same PID.
    // An extra descriptor forces reconciliation regardless of that layout.
    CritterSnapshot extraRebasedCritter = rejectionBaseline.critters.front();
    extraRebasedCritter.entityId = EntityId { unusedCritterId };
    extraRebasedCritter.frame = 0;
    rebasedNpcFrame.critters.push_back(extraRebasedCritter);
    if (!rejectsWithoutMutation(rebasedNpcFrame, "reconciled_npc_frame_out_of_bounds")) return false;
    WorldSnapshot newNpcFrame = rejectionBaseline;
    CritterSnapshot newCritter = newNpcFrame.critters.front();
    newCritter.entityId = EntityId { unusedCritterId };
    newCritter.tile = tile_num_in_direction(newCritter.tile, 0, 1);
    if (!hexGridTileIsValid(newCritter.tile)) return false;
    newCritter.frame = 0x7FFFFFFF;
    newCritter.whoHitMeId = {};
    newNpcFrame.critters.push_back(newCritter);
    ++newNpcFrame.actors.front().hitPoints;
    if (!rejectsWithoutMutation(newNpcFrame, "new_npc_frame_out_of_bounds")) return false;
    auto critterControl = [&](const WorldSnapshot& control, const char* reason) {
        std::vector<std::uint8_t> packet;
        if (encodeSnapshot(control, packet) != SnapshotError::None) return false;
        auto decoded = decodeSnapshot(packet);
        if (!decoded) return false;
        engineExecutionProbeBegin();
        bool applied = networkWorldApplySnapshot(decoded.snapshot);
        WorldSnapshot captured;
        bool capturePassed = networkWorldCaptureSnapshot({}, captured);
        auto expected = computeSnapshotDigest(control);
        auto actual = computeSnapshotDigest(captured);
        bool matched = applied && capturePassed && expected && actual
            && expected.digest.overall == actual.digest.overall;
        bool restored = networkWorldApplySnapshot(rejectionBaseline);
        WorldSnapshot restoredCapture;
        bool restoreCaptured = networkWorldCaptureSnapshot({}, restoredCapture);
        auto restoredDigest = computeSnapshotDigest(restoredCapture);
        EngineExecutionProbeCounts controlCounts = engineExecutionProbeEnd();
        bool preserved = restored && restoreCaptured && restoredDigest
            && rejectionDigest.digest.overall == restoredDigest.digest.overall
            && controlCounts.scriptProcedures == 0 && controlCounts.combatAttacks == 0
            && controlCounts.randomDraws == 0;
        std::fprintf(stderr, "NATIVE_SNAPSHOT_NPC_FRAME_CONTROL reason=%s applied=%d restored=%d.\n",
            reason, matched ? 1 : 0, preserved ? 1 : 0);
        return matched && preserved;
    };
    Object* hiddenCritter = session.entities().findObject(rejectionBaseline.critters.back().entityId);
    if (hiddenCritter == nullptr) return false;
    int hiddenFid = art_id(OBJ_TYPE_CRITTER, hiddenCritter->fid & 0xFFF, ANIM_TAKE_OUT, 0, 0);
    if (art_exists(hiddenFid)) return false;
    obj_change_fid(hiddenCritter, hiddenFid, nullptr);
    hiddenCritter->flags |= OBJECT_HIDDEN;
    WorldSnapshot hiddenPresentation;
    if (!networkWorldCaptureSnapshot({}, hiddenPresentation)
        || !critterControl(hiddenPresentation, "unchanged_hidden_missing_art")) return false;
    WorldSnapshot frameZero = rejectionBaseline;
    newCritter.frame = 0;
    newCritter.fid = art_id(OBJ_TYPE_CRITTER, newCritter.fid & 0xFFF, ANIM_TAKE_OUT, 0, 0);
    newCritter.objectFlags |= OBJECT_HIDDEN;
    frameZero.critters.push_back(newCritter);
    if (!critterControl(frameZero, "new_hidden_frame_zero")) return false;
    for (int frameSection = 0; frameSection < 3; ++frameSection) {
        WorldSnapshot invalidFrame = rejectionBaseline;
        ++invalidFrame.actors.front().hitPoints;
        invalidFrame.mapLocalVariables.push_back(123456);
        const char* reason = nullptr;
        if (frameSection == 0) {
            invalidFrame.doors.back().frame = 0x7FFFFFFF;
            invalidFrame.doors.back().open = true;
            reason = "door_frame_before_local_resize";
        } else if (frameSection == 1) {
            invalidFrame.scenery.back().frame = 0x7FFFFFFF;
            reason = "scenery_frame_before_local_resize";
        } else {
            invalidFrame.items.back().frame = 0x7FFFFFFF;
            reason = "item_frame_before_local_resize";
        }
        if (!rejectsWithoutMutation(invalidFrame, reason)) return false;
    }
    std::uint32_t unusedObjectId = 1;
    while (session.entities().contains(EntityId { unusedObjectId })) ++unusedObjectId;
    WorldSnapshot reconstructedSceneryFrame = rejectionBaseline;
    ++reconstructedSceneryFrame.actors.front().hitPoints;
    ScenerySnapshot addedScenery = reconstructedSceneryFrame.scenery.front();
    addedScenery.entityId = EntityId { unusedObjectId };
    addedScenery.tile = tile_num_in_direction(addedScenery.tile, 0, 1);
    if (!hexGridTileIsValid(addedScenery.tile)) return false;
    addedScenery.frame = 0x7FFFFFFF;
    reconstructedSceneryFrame.scenery.push_back(addedScenery);
    if (!rejectsWithoutMutation(reconstructedSceneryFrame, "new_scenery_frame_out_of_bounds")) return false;
    WorldSnapshot rebasedItemFrame = rejectionBaseline;
    ++rebasedItemFrame.actors.front().hitPoints;
    EntityId oldItemId = rebasedItemFrame.items.back().entityId;
    rebasedItemFrame.items.back().entityId = EntityId { unusedObjectId };
    rebasedItemFrame.items.back().frame = 0x7FFFFFFF;
    for (ItemSnapshot& state : rebasedItemFrame.items) {
        if (state.holderId == oldItemId) state.holderId = EntityId { unusedObjectId };
    }
    for (TimedEventSnapshot& state : rebasedItemFrame.timedEvents) {
        if (state.ownerId == oldItemId) state.ownerId = EntityId { unusedObjectId };
    }
    if (!rejectsWithoutMutation(rebasedItemFrame, "rebased_item_frame_out_of_bounds")) return false;
    auto objectPresentationControl = [&](const WorldSnapshot& control, const char* reason) {
        std::vector<std::uint8_t> packet;
        if (encodeSnapshot(control, packet) != SnapshotError::None) return false;
        auto decoded = decodeSnapshot(packet);
        if (!decoded) return false;
        engineExecutionProbeBegin();
        bool applied = networkWorldApplySnapshot(decoded.snapshot);
        WorldSnapshot captured;
        bool capturePassed = networkWorldCaptureSnapshot({}, captured);
        auto expected = computeSnapshotDigest(control);
        auto actual = computeSnapshotDigest(captured);
        bool matched = applied && capturePassed && expected && actual
            && expected.digest.overall == actual.digest.overall;
        bool restored = networkWorldApplySnapshot(rejectionBaseline);
        WorldSnapshot restoredCapture;
        bool restoreCaptured = networkWorldCaptureSnapshot({}, restoredCapture);
        auto restoredDigest = computeSnapshotDigest(restoredCapture);
        EngineExecutionProbeCounts controlCounts = engineExecutionProbeEnd();
        bool preserved = restored && restoreCaptured && restoredDigest
            && rejectionDigest.digest.overall == restoredDigest.digest.overall
            && controlCounts.scriptProcedures == 0 && controlCounts.combatAttacks == 0
            && controlCounts.randomDraws == 0;
        std::fprintf(stderr, "NATIVE_SNAPSHOT_OBJECT_FRAME_CONTROL reason=%s applied=%d restored=%d.\n",
            reason, matched ? 1 : 0, preserved ? 1 : 0);
        return matched && preserved;
    };
    WorldSnapshot validDoorFrame = rejectionBaseline;
    Object* nativeDoor = worldDoors.back().second;
    CacheEntry* doorHandle = nullptr;
    Art* doorFrames = art_ptr_lock(nativeDoor->fid, &doorHandle);
    if (doorFrames == nullptr) return false;
    validDoorFrame.doors.back().frame = art_frame_max_frame(doorFrames) - 1;
    art_ptr_unlock(doorHandle);
    validDoorFrame.doors.back().open = validDoorFrame.doors.back().frame != 0;
    if (!objectPresentationControl(validDoorFrame, "native_door_final_frame")) return false;
    Object* hiddenScenery = session.entities().findObject(rejectionBaseline.scenery.back().entityId);
    if (hiddenScenery == nullptr) return false;
    obj_change_fid(hiddenScenery, (OBJ_TYPE_SCENERY << 24) | 0xFFF, nullptr);
    hiddenScenery->flags |= OBJECT_HIDDEN;
    WorldSnapshot unchangedScenery;
    if (!networkWorldCaptureSnapshot({}, unchangedScenery)
        || !objectPresentationControl(unchangedScenery, "unchanged_hidden_scenery_missing_art")) return false;
    Object* hiddenItem = session.entities().findObject(rejectionBaseline.items.back().entityId);
    if (hiddenItem == nullptr) return false;
    obj_change_fid(hiddenItem, 0xFFF, nullptr);
    hiddenItem->flags |= OBJECT_HIDDEN;
    WorldSnapshot unchangedItem;
    if (!networkWorldCaptureSnapshot({}, unchangedItem)
        || !objectPresentationControl(unchangedItem, "unchanged_hidden_item_missing_art")) return false;
    WorldSnapshot newSceneryZero = rejectionBaseline;
    addedScenery.frame = 0;
    addedScenery.fid = (OBJ_TYPE_SCENERY << 24) | 0xFFF;
    addedScenery.objectFlags |= OBJECT_HIDDEN;
    newSceneryZero.scenery.push_back(addedScenery);
    if (!objectPresentationControl(newSceneryZero, "new_hidden_scenery_frame_zero")) return false;
    WorldSnapshot newItemZero = rejectionBaseline;
    ItemSnapshot addedItem = newItemZero.items.front();
    addedItem.entityId = EntityId { unusedObjectId };
    addedItem.holderId = {};
    addedItem.quantity = 1;
    addedItem.tile = tile_num_in_direction(rejectionBaseline.actors.front().tile, 0, 1);
    addedItem.elevation = rejectionBaseline.actors.front().elevation;
    if (!hexGridTileIsValid(addedItem.tile)) return false;
    addedItem.frame = 0;
    addedItem.fid = 0xFFF;
    addedItem.objectFlags |= OBJECT_HIDDEN;
    newItemZero.items.push_back(addedItem);
    WorldSnapshot invalidNewItemFrame = newItemZero;
    invalidNewItemFrame.items.back().frame = 0x7FFFFFFF;
    ++invalidNewItemFrame.actors.front().hitPoints;
    if (!rejectsWithoutMutation(invalidNewItemFrame, "new_item_frame_out_of_bounds")) return false;
    if (!objectPresentationControl(newItemZero, "new_hidden_item_frame_zero")) return false;
    std::fprintf(stderr, "NATIVE_SNAPSHOT_REJECTION_PREFLIGHT_PASS\n");

    WorldSnapshot sceneryBaseline;
    if (!networkWorldCaptureSnapshot({}, sceneryBaseline) || sceneryBaseline.scenery.empty()) return false;
    WorldSnapshot changedScenery = sceneryBaseline;
    std::uint32_t nextSceneryId = 1;
    auto includeIds = [&nextSceneryId](const auto& entries) {
        for (const auto& entry : entries) nextSceneryId = std::max(nextSceneryId, entry.first.value + 1);
    };
    includeIds(worldScenery);
    includeIds(worldDoors);
    includeIds(worldExitGrids);
    includeIds(worldCritters);
    includeIds(worldItems);
    for (const ActorSnapshot& actor : sceneryBaseline.actors) {
        nextSceneryId = std::max(nextSceneryId, actor.entityId.value + 1);
    }
    ScenerySnapshot extraScenery = changedScenery.scenery.front();
    extraScenery.entityId = EntityId { nextSceneryId++ };
    changedScenery.scenery.push_back(extraScenery);
    extraScenery.entityId = EntityId { nextSceneryId++ };
    changedScenery.scenery.push_back(extraScenery);
    changedScenery.scenery.erase(changedScenery.scenery.begin());
    engineExecutionProbeBegin();
    bool sceneryApplied = networkWorldApplySnapshot(changedScenery);
    WorldSnapshot reconstructedScenery;
    bool sceneryCaptured = sceneryApplied && networkWorldCaptureSnapshot({}, reconstructedScenery);
    bool sceneryRestored = networkWorldApplySnapshot(sceneryBaseline);
    auto expectedSceneryDigest = computeSnapshotDigest(changedScenery);
    auto actualSceneryDigest = computeSnapshotDigest(reconstructedScenery);
    EngineExecutionProbeCounts sceneryCounts = engineExecutionProbeEnd();
    if (!sceneryCaptured || !sceneryRestored
        || !expectedSceneryDigest || !actualSceneryDigest
        || expectedSceneryDigest.digest.scenery != actualSceneryDigest.digest.scenery
        || sceneryCounts.scriptProcedures != 0
        || sceneryCounts.combatAttacks != 0
        || sceneryCounts.randomDraws != 0) {
        std::fprintf(stderr, "Multiplayer scenery reconstruction probe failed: applied=%d captured=%d restored=%d scripts=%u attacks=%u rng=%u.\n",
            sceneryApplied, sceneryCaptured, sceneryRestored, sceneryCounts.scriptProcedures,
            sceneryCounts.combatAttacks, sceneryCounts.randomDraws);
        return false;
    }
    std::fprintf(stderr, "NATIVE_SCENERY_RECONSTRUCTION_PASS added=2 removed=1 restored=1 replica_scripts=0\n");

    engineExecutionProbeBegin();
    bool doorApplied = networkWorldApplyPeerDoorUse(DoorUseStartedEvent {
        hostActorId,
        scriptedDoorId,
        obj_is_open(scriptedDoor) != 0,
        obj_is_locked(scriptedDoor),
        scriptedDoor->frame,
    });
    EngineExecutionProbeCounts doorCounts = engineExecutionProbeEnd();
    if (!doorApplied
        || doorCounts.scriptProcedures != 0
        || doorCounts.combatAttacks != 0
        || doorCounts.randomDraws != 0) {
        std::fprintf(stderr, "Multiplayer authority probe: guest door applied=%d scripts=%u attacks=%u rng=%u.\n",
            doorApplied ? 1 : 0,
            doorCounts.scriptProcedures,
            doorCounts.combatAttacks,
            doorCounts.randomDraws);
        return false;
    }

    engineExecutionProbeBegin();
    bool attackApplied = networkWorldApplyPeerAttack(AttackStartedEvent {
        hostActorId,
        targetId,
        HIT_MODE_PUNCH,
        HIT_LOCATION_TORSO,
    });
    EngineExecutionProbeCounts attackCounts = engineExecutionProbeEnd();
    if (!attackApplied
        || attackCounts.scriptProcedures != 0
        || attackCounts.combatAttacks != 0
        || attackCounts.randomDraws != 0) {
        std::fprintf(stderr, "Multiplayer authority probe: guest attack applied=%d scripts=%u attacks=%u rng=%u.\n",
            attackApplied ? 1 : 0,
            attackCounts.scriptProcedures,
            attackCounts.combatAttacks,
            attackCounts.randomDraws);
        return false;
    }
    return true;
}

EntityId networkWorldCombatSmokeTarget()
{
    return !worldCritters.empty() ? worldCritters.front().first : EntityId {};
}

int networkWorldCombatSmokeMoveTile()
{
    Object* guest = networkWorldPlayerActor(kGuestPlayerId);
    if (guest == nullptr) return -1;
    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        int tile = tile_num_in_direction(guest->tile, rotation, 1);
        if (hexGridTileIsValid(tile)
            && obj_blocking_at(guest, tile, guest->elevation) == nullptr) {
            return tile;
        }
    }
    return -1;
}

bool networkWorldCombatSmokeTargetDead()
{
    return !worldCritters.empty() && worldCritters.front().second != nullptr
        && critter_is_dead(worldCritters.front().second);
}

EntityId networkWorldCombatSmokeItem()
{
    Object* guest = networkWorldPlayerActor(kGuestPlayerId);
    if (guest == nullptr) return {};
    for (const auto& entry : worldItems) {
        if (entry.second != nullptr && entry.second->owner == guest
            && entry.second->pid == PROTO_ID_STIMPACK) {
            return entry.first;
        }
    }
    return {};
}

bool networkWorldCombatSmokeActorHealed()
{
    Object* guest = networkWorldPlayerActor(kGuestPlayerId);
    return guest != nullptr
        && critter_get_hits(guest)
            > std::max(1, stat_level(guest, STAT_MAXIMUM_HIT_POINTS) - 20);
}

bool networkWorldPrepareCombatItemSmoke()
{
    Object* guest = networkWorldPlayerActor(kGuestPlayerId);
    if (worldMode != NetworkLaunchMode::Host || guest == nullptr) return false;
    Object* item = nullptr;
    if (obj_pid_new(&item, PROTO_ID_STIMPACK) == -1 || item == nullptr) return false;
    if (obj_disconnect(item, nullptr) == -1
        || item_add_force(guest, item, 1) != 0) {
        obj_erase_object(item, nullptr);
        return false;
    }
    if (!registerItem(item) || !setInventoryQuantity(guest, item, 1)) return false;
    guest->data.critter.hp = std::max(1,
        stat_level(guest, STAT_MAXIMUM_HIT_POINTS) - 20);
    return true;
}

EntityId networkWorldCombatSmokeWeapon()
{
    Object* guest = networkWorldPlayerActor(kGuestPlayerId);
    if (guest == nullptr) return {};
    for (const auto& entry : worldItems) {
        if (entry.second != nullptr && entry.second->owner == guest
            && item_get_type(entry.second) == ITEM_TYPE_WEAPON
            && (entry.second->flags & OBJECT_IN_RIGHT_HAND) != 0) {
            return entry.first;
        }
    }
    return {};
}

bool networkWorldCombatSmokeWeaponLoaded()
{
    Object* weapon = networkWorldFindObject(networkWorldCombatSmokeWeapon());
    return weapon != nullptr && item_w_curr_ammo(weapon) > 0;
}

int networkWorldCombatSmokeAmmoUnits()
{
    Object* guest = networkWorldPlayerActor(kGuestPlayerId);
    Object* weapon = networkWorldFindObject(networkWorldCombatSmokeWeapon());
    if (guest == nullptr || weapon == nullptr) return -1;
    int units = item_w_curr_ammo(weapon);
    Inventory* inventory = &guest->data.inventory;
    for (int index = 0; index < inventory->length; index++) {
        InventoryItem* entry = &inventory->items[index];
        if (entry->item == nullptr || item_get_type(entry->item) != ITEM_TYPE_AMMO
            || entry->quantity <= 0) continue;
        units += item_w_curr_ammo(entry->item)
            + (entry->quantity - 1) * item_w_max_ammo(entry->item);
    }
    return units;
}

bool networkWorldPrepareHostWeaponAttackSmoke()
{
    Object* host = networkWorldPlayerActor(kHostPlayerId);
    Object* target = networkWorldFindObject(networkWorldCombatSmokeTarget());
    if (host == nullptr || target == nullptr) return false;
    target->data.critter.hp = 100;
    for (int rotation = 0; rotation < ROTATION_COUNT; ++rotation) {
        int tile = tile_num_in_direction(target->tile, rotation, 1);
        if (tile >= 0 && obj_blocking_at(host, tile, target->elevation) == nullptr
            && obj_move_to_tile(host, tile, target->elevation, nullptr) == 0) return true;
    }
    return false;
}

std::optional<EntityId> networkWorldPrepareCombatLootSmoke()
{
    Object* actor = networkWorldPlayerActor(kGuestPlayerId);
    if (actor == nullptr) return std::nullopt;
    for (const auto& entry : worldItems) {
        Object* item = entry.second;
        if (item->pid == 211 && item->owner == nullptr && item->tile == actor->tile
            && item->elevation == actor->elevation) return entry.first;
    }
    if (worldMode != NetworkLaunchMode::Host) return std::nullopt;
    Object* container = nullptr;
    if (obj_pid_new(&container, 211) != 0 || container == nullptr
        || obj_move_to_tile(container, actor->tile, actor->elevation, nullptr) != 0)
        return std::nullopt;
    auto registered = registerItem(container);
    if (!registered) return std::nullopt;
    for (int pid : { 4, 30 }) {
        Object* item = nullptr;
        if (obj_pid_new(&item, pid) != 0 || item == nullptr
            || obj_disconnect(item, nullptr) != 0 || item_add_force(container, item, 1) != 0
            || !registerItem(item)) return std::nullopt;
    }
    return registered.entityId;
}

std::optional<EntityId> networkWorldPrepareGroundContainerLootSmokeTest(PlayerId looterId)
{
    Object* actor = networkWorldPlayerActor(looterId);
    if (actor == nullptr) return std::nullopt;
    for (const auto& entry : worldItems) {
        Object* container = entry.second;
        if (container->pid != 211 || container->owner != nullptr || item_get_type(container) != ITEM_TYPE_CONTAINER) continue;
        if (worldMode == NetworkLaunchMode::Host) {
            anim_stop();
            for (int rotation = 0; rotation < ROTATION_COUNT; ++rotation) {
                int tile = tile_num_in_direction(container->tile, rotation, 5);
                if (!hexGridTileIsValid(tile) || obj_blocking_at(actor, tile, container->elevation) != nullptr) continue;
                std::array<unsigned char, kMaximumMovementPathLength> path;
                int flags = container->flags;
                container->flags |= OBJECT_HIDDEN;
                int pathLength = make_path(actor, tile, container->tile, path.data(), 0);
                container->flags = flags;
                if (hexGridTileIsValid(tile) && obj_blocking_at(actor, tile, container->elevation) == nullptr
                    && pathLength > 0
                    && obj_move_to_tile(actor, tile, container->elevation, nullptr) == 0) break;
            }
            bool wasLocked = obj_is_locked(container);
            if (obj_lock(container) != 0) return std::nullopt;
            bool rejected = commandExecutor.loot(actor, container) == CommandExecutionStatus::InvalidAction;
            if (!wasLocked) obj_unlock(container);
            if (!rejected) return std::nullopt;
            std::fprintf(stderr, "NATIVE_CONTAINER_LOCKED_REJECT_PASS target=%u\n", entry.first.value);
        }
        return lootTargetIsValid(actor, container) && obj_dist(actor, container) == 5
            ? std::optional<EntityId>(entry.first) : std::nullopt;
    }
    if (worldMode == NetworkLaunchMode::Host) {
        std::fprintf(stderr, "NATIVE_CONTAINER_FIXTURE_MISSING map=%s items=%zu\n", map_data.name, worldItems.size());
        for (const auto& entry : worldItems) {
            if (entry.second->owner == nullptr && item_get_type(entry.second) == ITEM_TYPE_CONTAINER)
                std::fprintf(stderr, "NATIVE_CONTAINER_CANDIDATE id=%u pid=%d tile=%d\n", entry.first.value, entry.second->pid, entry.second->tile);
        }
    }
    return std::nullopt;
}

static bool nestedInventoryActionsSmokeTest(Object* actor, std::uint64_t revision)
{
    Object* bag = nullptr;
    Object* inner = nullptr;
    Object* drug = nullptr;
    Object* pistol = nullptr;
    Object* ammo = nullptr;
    int hp = actor->data.critter.hp;
    int ap = actor->data.critter.combat.ap;
    bool passed = false;
    auto create = [](Object** object, int pid) {
        return obj_pid_new(object, pid) == 0 && *object != nullptr
            && obj_disconnect(*object, nullptr) == 0;
    };
    if (create(&bag, 211) && create(&inner, 211) && create(&drug, 40)
        && create(&pistol, 8) && create(&ammo, 29)
        && item_add_force(actor, bag, 1) == 0 && item_add_force(bag, inner, 1) == 0
        && item_add_force(inner, drug, 2) == 0 && item_add_force(inner, pistol, 2) == 0
        && item_add_force(inner, ammo, 2) == 0 && registerUntrackedInventory(actor)) {
        item_w_set_curr_ammo(pistol, 12);
        item_w_set_curr_ammo(ammo, item_w_max_ammo(ammo));
        auto drugId = networkWorldFindEntity(drug).value_or(EntityId {});
        auto pistolId = networkWorldFindEntity(pistol).value_or(EntityId {});
        auto actorId = networkWorldFindEntity(actor).value_or(EntityId {});
        auto bagId = networkWorldFindEntity(bag).value_or(EntityId {});
        auto innerId = networkWorldFindEntity(inner).value_or(EntityId {});
        InventoryTransferCommand transfer { innerId, bagId, drugId, 1, 2, {}, revision };
        transfer.turnRevision = revision + 1;
        bool staleRejected = commandExecutor.transferInventory(actor, inner, bag, drug, transfer).status
            == CommandExecutionStatus::InvalidAction && item_count(inner, drug) == 2;
        transfer.turnRevision = revision;
        auto access = openInventories.find(actorId);
        std::optional<std::uint64_t> savedAccess = access != openInventories.end()
            ? std::optional<std::uint64_t>(access->second) : std::nullopt;
        openInventories.erase(actorId);
        bool closedRejected = commandExecutor.transferInventory(actor, inner, bag, drug, transfer).status
            == CommandExecutionStatus::InvalidAction && item_count(inner, drug) == 2;
        if (savedAccess) openInventories[actorId] = *savedAccess;
        auto moved = commandExecutor.transferInventory(actor, inner, bag, drug, transfer);
        bool movedToParent = moved.status == CommandExecutionStatus::Applied
            && isValid(moved.remainderItemId) && drug->owner == bag && item_count(bag, drug) == 1;
        transfer.sourceId = bagId;
        transfer.destinationId = innerId;
        transfer.sourceQuantity = 1;
        auto returned = commandExecutor.transferInventory(actor, bag, inner, drug, transfer);
        bool movedBack = returned.status == CommandExecutionStatus::Applied
            && drug->owner == inner && item_count(inner, drug) == 2;
        bool transferPassed = staleRejected && closedRejected && movedToParent && movedBack
            && actor->data.critter.combat.ap == ap;
        std::fprintf(stderr, "NATIVE_COMBAT_CONTAINER_TRANSFER_%s stale_rejected=%d closed_rejected=%d split=%d return=%d ap_unchanged=%d\n",
            transferPassed ? "PASS" : "FAIL", staleRejected, closedRejected, movedToParent, movedBack,
            actor->data.critter.combat.ap == ap);

        auto act = [&](Object* acting, EntityId id, InventoryAction action, EntityId ammoId = {}) {
            return commandExecutor.inventoryAction(acting, InventoryActionCommand {
                revision, id, action, ammoId, 1 });
        };
        Object* foreign = networkWorldPlayerActor(actor == networkWorldPlayerActor(kHostPlayerId)
            ? kGuestPlayerId : kHostPlayerId);
        actor->data.critter.hp = std::max(1, stat_level(actor, STAT_MAXIMUM_HIT_POINTS) - 20);
        int wounded = actor->data.critter.hp;
        bool rejected = act(foreign, drugId, InventoryAction::Use) == CommandExecutionStatus::InvalidAction;
        bool used = act(actor, drugId, InventoryAction::Use) == CommandExecutionStatus::Applied;
        // Consuming a representative leaves a newly registered stack copy.
        drug = nullptr;
        int drugs = 0;
        for (int index = 0; index < inner->data.inventory.length; ++index) {
            auto entry = inner->data.inventory.items[index];
            if (entry.item->pid == 40) drugs += entry.quantity;
        }
        bool unloaded = act(actor, pistolId, InventoryAction::Unload) == CommandExecutionStatus::Applied
            && pistol->owner == inner && item_w_curr_ammo(pistol) == 0
            && item_count(inner, pistol) == 1;
        // Native ammo stacking replaces its representative when unloading.
        ammo = nullptr;
        int beforeUnits = 0;
        for (int index = 0; index < inner->data.inventory.length; ++index) {
            auto entry = inner->data.inventory.items[index];
            if (entry.item->pid == 29) {
                ammo = entry.item;
                beforeUnits += item_w_curr_ammo(entry.item)
                    + (entry.quantity - 1) * item_w_max_ammo(entry.item);
            }
        }
        auto ammoId = networkWorldFindEntity(ammo).value_or(EntityId {});
        bool loaded = act(actor, pistolId, InventoryAction::Reload, ammoId) == CommandExecutionStatus::Applied;
        int afterUnits = item_w_curr_ammo(pistol);
        for (int index = 0; index < inner->data.inventory.length; ++index) {
            auto entry = inner->data.inventory.items[index];
            if (entry.item->pid == 29) afterUnits += item_w_curr_ammo(entry.item)
                + (entry.quantity - 1) * item_w_max_ammo(entry.item);
        }
        bool roundsLoaded = item_w_curr_ammo(pistol) == 12;
        auto loadedId = networkWorldFindEntity(pistol).value_or(EntityId {});
        bool dropped = act(actor, loadedId, InventoryAction::Drop) == CommandExecutionStatus::Applied
            && pistol->owner == nullptr && pistol->tile == actor->tile
            && pistol->elevation == actor->elevation;
        if (pistol->owner == nullptr) obj_erase_object(pistol, nullptr);
        passed = transferPassed && rejected && used && drugs == 1 && actor->data.critter.hp > wounded
            && unloaded && loaded && beforeUnits == afterUnits && roundsLoaded
            && dropped && actor->data.critter.combat.ap == ap;
        // All fixture contents remain under the two private holders.
        pistol = nullptr;
        ammo = nullptr;
    }
    actor->data.critter.hp = hp;
    // Remove children explicitly so no inventory representative remains live
    // in the entity registry after its holder is freed.
    if (inner != nullptr) {
        while (inner->data.inventory.length > 0) {
            auto entry = inner->data.inventory.items[0];
            item_remove_mult(inner, entry.item, entry.quantity);
            obj_erase_object(entry.item, nullptr);
        }
        if (inner->owner != nullptr) item_remove_mult(inner->owner, inner, 1);
        obj_erase_object(inner, nullptr);
    }
    if (bag != nullptr) {
        if (bag->owner != nullptr) item_remove_mult(bag->owner, bag, 1);
        obj_erase_object(bag, nullptr);
    }
    std::fprintf(stderr, "NATIVE_NESTED_INVENTORY_ACTIONS_%s use=1 unload_stack=1 reload=1 drop=1 ammo_conserved=1 foreign_rejected=1 ap_unchanged=1\n",
        passed ? "PASS" : "FAIL");
    return passed;
}

static bool combatScannerSmoke(Object* actor, std::uint64_t revision)
{
    int ap = actor->data.critter.combat.ap;
    EntityId actorId = networkWorldFindEntity(actor).value_or(EntityId {});
    auto savedAccess = openInventories.at(actorId);
    Object* originalHand = inven_right_hand(actor);
    Object* sensor = nullptr;
    bool passed = obj_pid_new(&sensor, PROTO_ID_MOTION_SENSOR) == 0 && sensor != nullptr;
    if (passed) {
        obj_disconnect(sensor, nullptr); item_m_set_charges(sensor, 3);
        passed = item_add_force(actor, sensor, 1) == 0 && registerItem(sensor);
    }
    if (passed) {
        if (originalHand != nullptr) originalHand->flags &= ~OBJECT_IN_RIGHT_HAND;
        sensor->flags |= OBJECT_IN_RIGHT_HAND;
        auto id = networkWorldFindEntity(sensor).value_or(EntityId {});
        InventoryActionCommand scan { revision, id, InventoryAction::Scan };
        openInventories.erase(actorId);
        scan.turnRevision++;
        passed = commandExecutor.inventoryAction(actor, scan) == CommandExecutionStatus::InvalidAction && passed;
        scan.turnRevision = revision;
        passed = commandExecutor.inventoryAction(actor, scan) == CommandExecutionStatus::Applied
            && item_m_curr_charges(sensor) == 2 && actor->data.critter.combat.ap == ap && passed;
        scan.action = InventoryAction::Use;
        passed = commandExecutor.inventoryAction(actor, scan) == CommandExecutionStatus::InvalidAction && passed;
        openInventories[actorId] = savedAccess;
        passed = commandExecutor.inventoryAction(actor, scan) == CommandExecutionStatus::Applied
            && item_m_curr_charges(sensor) == 1 && actor->data.critter.combat.ap == ap && passed;
        passed = commandExecutor.combatItem(actor, CombatItemCommand { revision, id }) == CommandExecutionStatus::Applied
            && item_m_curr_charges(sensor) == 0 && actor->data.critter.combat.ap == ap - 2 && passed;
        passed = commandExecutor.combatItem(actor, CombatItemCommand { revision, id }) == CommandExecutionStatus::InvalidAction
            && actor->data.critter.combat.ap == ap - 2 && passed;
    }
    openInventories[actorId] = savedAccess;
    actor->data.critter.combat.ap = ap;
    if (sensor != nullptr) {
        if (sensor->owner != nullptr) item_remove_mult(sensor->owner, sensor, item_count(sensor->owner, sensor));
        obj_erase_object(sensor, nullptr);
    }
    if (originalHand != nullptr) originalHand->flags |= OBJECT_IN_RIGHT_HAND;
    std::fprintf(stderr, "NATIVE_COMBAT_SCANNER_%s stale_rejected=1 automap_ap=0 inventory_ap=0 hud_ap=2 empty_rejected=1\n", passed ? "PASS" : "FAIL");
    return passed;
}

static bool combatExplosiveTimerSmoke(Object* actor, std::uint64_t revision)
{
    int originalAp = actor->data.critter.combat.ap;
    auto actorId = networkWorldFindEntity(actor).value_or(EntityId {});
    auto access = openInventories.find(actorId);
    if (access == openInventories.end()) return false;
    auto savedAccess = access->second;
    Object* bomb = nullptr;
    bool passed = obj_pid_new(&bomb, PROTO_ID_DYNAMITE_I) == 0 && bomb != nullptr
        && obj_disconnect(bomb, nullptr) == 0 && item_add_force(actor, bomb, 2) == 0 && registerItem(bomb);
    if (passed) {
        auto id = networkWorldFindEntity(bomb).value_or(EntityId {});
        InventoryActionCommand arm { revision, id, InventoryAction::Use, {}, 1, 120 };
        arm.turnRevision++;
        passed = commandExecutor.inventoryAction(actor, arm) == CommandExecutionStatus::InvalidAction && passed;
        arm.turnRevision = revision;
        openInventories.erase(actorId);
        passed = commandExecutor.inventoryAction(actor, arm) == CommandExecutionStatus::InvalidAction && passed;
        openInventories[actorId] = savedAccess;
        passed = commandExecutor.inventoryAction(actor, arm) == CommandExecutionStatus::Applied
            && actor->data.critter.combat.ap == originalAp && item_count(actor, bomb) == 1 && passed;
        queue_remove(bomb);
        item_remove_mult(actor, bomb, 1);
        obj_erase_object(bomb, nullptr);
        bomb = nullptr;
        Object* remaining = session.entities().findObject(lastSplitEntityId);
        // Removal of the armed singleton does not create a new split, so this
        // identity still names the unarmed remainder from the initial arming.
        passed = remaining != nullptr && remaining->pid == PROTO_ID_DYNAMITE_I && passed;
        if (remaining != nullptr) {
            auto remainingId = networkWorldFindEntity(remaining).value_or(EntityId {});
            passed = commandExecutor.combatItem(actor, CombatItemCommand { revision + 1, remainingId, {}, 120 })
                == CommandExecutionStatus::InvalidAction && passed;
            passed = commandExecutor.combatItem(actor, CombatItemCommand { revision, remainingId, {}, 120 })
                == CommandExecutionStatus::Applied && actor->data.critter.combat.ap == originalAp - 2 && passed;
            queue_remove(remaining);
            item_remove_mult(actor, remaining, 1);
            obj_erase_object(remaining, nullptr);
        }
    }
    if (bomb != nullptr) {
        queue_remove(bomb);
        if (bomb->owner != nullptr) item_remove_mult(bomb->owner, bomb, item_count(bomb->owner, bomb));
        obj_erase_object(bomb, nullptr);
    }
    actor->data.critter.combat.ap = originalAp;
    std::fprintf(stderr, "NATIVE_COMBAT_EXPLOSIVE_TIMER_%s stale_rejected=1 closed_inventory_rejected=1 inventory_ap=0 hud_ap=2\n", passed ? "PASS" : "FAIL");
    return passed;
}

static bool combatInteractionSmoke()
{
    CombatTurnState savedTurns = combatTurns.snapshot(combatClockMilliseconds());
    int savedFreeMove = combat_free_move;
    std::size_t savedEvents = deferredEvents.size();
    bool passed = true;
    for (PlayerId id : session.players().playerIds()) {
        Object* actor = networkWorldPlayerActor(id);
        auto* player = playerStateForActor(actor);
        if (actor == nullptr || player == nullptr) return false;
        CombatTurnState turn = savedTurns;
        auto found = std::find_if(turn.initiative.begin(), turn.initiative.end(),
            [&](const auto& entry) { return entry.actorId == player->actorId; });
        if (found == turn.initiative.end()) return false;
        turn.activeIndex = static_cast<unsigned>(found - turn.initiative.begin());
        if (!combatTurns.restore(turn, combatClockMilliseconds())) return false;
        ScopedActingPlayerContext context(*player, actor);
        ScopedLocalPlayerBinding binding(actor);
        int savedAp = actor->data.critter.combat.ap;
        int savedFid = actor->fid;
        int originalTile = actor->tile;
        auto actorId = player->actorId;
        auto previousAccess = openInventories.find(actorId);
        std::optional<std::uint64_t> savedAccess = previousAccess == openInventories.end()
            ? std::nullopt : std::optional<std::uint64_t>(previousAccess->second);
        openInventories.erase(actorId);
        combat_free_move = 0;
        obj_change_fid(actor, art_id(OBJ_TYPE_CRITTER, savedFid & 0xFFF, ANIM_STAND, 0, actor->rotation + 1), nullptr);
        Object* corpse = nullptr;
        Object* resource = nullptr;
        Object* ground = nullptr;
        Object* door = nullptr;
        bool prepared = obj_pid_new(&corpse, 0x1000000) == 0 && corpse != nullptr
            && obj_move_to_tile(corpse, originalTile, actor->elevation, nullptr) == 0
            && session.registerWorldObject(corpse)
            && obj_pid_new(&resource, 213) == 0 && resource != nullptr
            && obj_disconnect(resource, nullptr) == 0 && item_add_force(corpse, resource, 1) == 0
            && registerItem(resource);
        bool loot = false, transfer = false, closed = false, pickup = false, doorUsed = false, approached = false;
        if (prepared) {
            corpse->data.critter.combat.results |= DAM_DEAD;
            auto corpseId = networkWorldFindEntity(corpse).value_or(EntityId {});
            auto resourceId = networkWorldFindEntity(resource).value_or(EntityId {});
            actor->data.critter.combat.ap = 12;
            bool stale = commandExecutor.loot(actor, corpse, turn.revision + 1) == CommandExecutionStatus::InvalidAction
                && actor->data.critter.combat.ap == 12;
            actor->data.critter.combat.ap = 2;
            bool insufficient = commandExecutor.loot(actor, corpse, turn.revision) == CommandExecutionStatus::InvalidAction
                && actor->data.critter.combat.ap == 2 && !commandExecutor.combatLootAccess(actor, turn.revision);
            actor->data.critter.combat.ap = 12;
            loot = stale && insufficient && commandExecutor.loot(actor, corpse, turn.revision) == CommandExecutionStatus::Applied
                && actor->data.critter.combat.ap == 9 && commandExecutor.combatLootAccess(actor, turn.revision);
            loot = loot && commandExecutor.loot(actor, corpse, turn.revision, true) == CommandExecutionStatus::Applied
                && actor->data.critter.combat.ap == 9;
            for (PlayerId otherId : session.players().playerIds()) {
                if (otherId == id) continue;
                Object* other = networkWorldPlayerActor(otherId);
                int otherAp = other->data.critter.combat.ap;
                loot = commandExecutor.loot(other, corpse, turn.revision) == CommandExecutionStatus::InvalidAction
                    && other->data.critter.combat.ap == otherAp && loot;
            }
            InventoryTransferCommand take { corpseId, actorId, resourceId, 1, 1, {}, turn.revision };
            transfer = loot && commandExecutor.transferInventory(actor, corpse, actor, resource, take).status == CommandExecutionStatus::Applied
                && resource->owner == actor && actor->data.critter.combat.ap == 9;
            EquipmentCommand close; close.turnRevision = turn.revision; close.action = EquipmentAction::CloseInventory;
            closed = commandExecutor.setEquipment(actor, close) == CommandExecutionStatus::Applied
                && !commandExecutor.combatLootAccess(actor, turn.revision)
                && commandExecutor.loot(actor, corpse, turn.revision, true) == CommandExecutionStatus::InvalidAction;
            take.sourceId = actorId; take.destinationId = corpseId;
            closed = closed && commandExecutor.transferInventory(actor, actor, corpse, resource, take).status == CommandExecutionStatus::InvalidAction;
            EquipmentCommand open; open.turnRevision = turn.revision; open.action = EquipmentAction::OpenInventory;
            closed = closed && commandExecutor.setEquipment(actor, open) == CommandExecutionStatus::Applied
                && actor->data.critter.combat.ap == 5;
            commandExecutor.setEquipment(actor, close);
            // Check the native route and its exact movement charge, including detours.
            for (int distance = 2; distance <= 6 && !approached; ++distance) {
                for (int rotation = 0; rotation < ROTATION_COUNT && !approached; ++rotation) {
                    int remote = tile_num_in_direction(originalTile, rotation, distance);
                    std::array<unsigned char, kMaximumMovementPathLength> path;
                    if (!hexGridTileIsValid(remote) || obj_blocking_at(actor, remote, actor->elevation) != nullptr) continue;
                    int flags = corpse->flags; corpse->flags |= OBJECT_HIDDEN;
                    int length = make_path(actor, remote, originalTile, path.data(), 0);
                    corpse->flags = flags;
                    if (length <= 1 || length > 8) continue;
                    obj_move_to_tile(actor, remote, actor->elevation, nullptr);
                    actor->data.critter.combat.ap = 40;
                    int expectedAp = 40 - critter_compute_ap_from_distance(actor, length - 1) - 3;
                    auto result = commandExecutor.loot(actor, corpse, turn.revision);
                    approached = result == CommandExecutionStatus::Applied && obj_dist(actor, corpse) <= 1
                        && actor->data.critter.combat.ap == expectedAp;
                    std::fprintf(stderr, "COMBAT_INTERACTION_APPROACH player=%u status=%d ap=%d expected=%d length=%d\n",
                        id.value, static_cast<int>(result), actor->data.critter.combat.ap, expectedAp, length);
                    commandExecutor.setEquipment(actor, close);
                    obj_move_to_tile(actor, originalTile, actor->elevation, nullptr);
                }
            }
            if (obj_pid_new(&ground, PROTO_ID_FLARE) == 0 && ground != nullptr) {
                ground->flags |= OBJECT_USED;
                obj_move_to_tile(ground, originalTile, actor->elevation, nullptr);
                auto groundId = session.registerWorldObject(ground).entityId;
                actor->data.critter.combat.ap = 12;
                bool stalePickup = commandExecutor.pickup(actor, ground, turn.revision + 1) == CommandExecutionStatus::InvalidAction;
                pickup = stalePickup && commandExecutor.pickup(actor, ground, turn.revision) == CommandExecutionStatus::Applied
                    && session.entities().findObject(groundId) == ground && ground->owner == actor
                    && actor->data.critter.combat.ap == 9;
            }
            if (!worldDoors.empty() && obj_pid_new(&door, worldDoors.begin()->second->pid) == 0 && door != nullptr) {
                obj_move_to_tile(door, originalTile, actor->elevation, nullptr);
                auto doorId = session.registerWorldObject(door).entityId;
                actor->data.critter.combat.ap = 12;
                auto used = commandExecutor.useDoor(actor, door, turn.revision);
                doorUsed = used.status == CommandExecutionStatus::Applied && session.entities().findObject(doorId) == door
                    && actor->data.critter.combat.ap == 9 && used.open == (obj_is_open(door) != 0)
                    && used.frame == door->frame;
            }
        }
        std::fprintf(stderr, "COMBAT_INTERACTION_%s player=%u loot=%d transfer=%d close=%d approach=%d pickup=%d door=%d\n",
            loot && transfer && closed && approached && pickup && doorUsed ? "PASS" : "FAIL", id.value,
            loot, transfer, closed, approached, pickup, doorUsed);
        passed = loot && transfer && closed && approached && pickup && doorUsed && passed;
        for (Object* object : { resource, ground, corpse, door }) {
            if (object == nullptr || !session.entities().findEntity(object).has_value()) continue;
            if (object->owner != nullptr) item_remove_mult(object->owner, object, item_count(object->owner, object));
            obj_erase_object(object, nullptr);
        }
        openLootTurns.erase(actorId); activeLootTargets.erase(actor);
        openInventories.erase(actorId); if (savedAccess) openInventories[actorId] = *savedAccess;
        actor->data.critter.combat.ap = savedAp;
        obj_change_fid(actor, savedFid, nullptr);
        obj_move_to_tile(actor, originalTile, actor->elevation, nullptr);
    }
    combat_free_move = savedFreeMove;
    while (deferredEvents.size() > savedEvents) deferredEvents.pop_back();
    return combatTurns.restore(savedTurns, combatClockMilliseconds()) && passed;
}

bool networkWorldRunCombatInventoryDropSmokeTest()
{
    Object* actor = networkWorldPlayerActor(kHostPlayerId);
    auto* player = playerStateForActor(actor);
    if (player == nullptr || inven_worn(actor) != nullptr) return false;
    ScopedActingPlayerContext context(*player, actor);
    ScopedLocalPlayerBinding binding(actor);
    CharacterBuild savedBuild = player->build;
    int savedFid = actor->fid;
    int ap = actor->data.critter.combat.ap;
    std::unordered_set<std::uint32_t> originalItems;
    for (const auto& entry : worldItems) originalItems.insert(entry.first.value);
    auto fixtureItem = [&](int pid, int quantity) {
        Object* item = nullptr;
        if (obj_pid_new(&item, pid) == -1 || item == nullptr) return static_cast<Object*>(nullptr);
        item->flags |= OBJECT_USED; // Keep test stacks separate from the player's possessions.
        if (obj_disconnect(item, nullptr) == -1 || item_add_force(actor, item, quantity) != 0 || !registerItem(item)) {
            obj_erase_object(item, nullptr);
            return static_cast<Object*>(nullptr);
        }
        return item;
    };
    auto drop = [&](Object* item, std::uint32_t quantity, std::uint64_t revision) {
        return commandExecutor.inventoryAction(actor, InventoryActionCommand {
            revision, networkWorldFindEntity(item).value_or(EntityId {}), InventoryAction::Drop, {}, quantity });
    };
    Object* flares = fixtureItem(PROTO_ID_FLARE, 3);
    Object* caps = fixtureItem(PROTO_ID_MONEY, 10);
    Object* armor = fixtureItem(1, 1); // Leather armor.
    bool passed = flares != nullptr && caps != nullptr && armor != nullptr;
    std::uint64_t revision = combatTurns.revision();
    passed = passed && combatInteractionSmoke() && combatScannerSmoke(actor, revision) && combatExplosiveTimerSmoke(actor, revision)
        && nestedInventoryActionsSmokeTest(actor, revision);
    if (passed) {
        passed = drop(flares, 2, revision + 1) == CommandExecutionStatus::InvalidAction
            && item_count(actor, flares) == 3 && passed;
        passed = drop(flares, 4, revision) == CommandExecutionStatus::InvalidAction
            && item_count(actor, flares) == 3 && passed;
        passed = drop(flares, 2, revision) == CommandExecutionStatus::Applied && passed;
        int onGround = 0;
        int remaining = 0;
        for (const auto& entry : worldItems) {
            Object* item = entry.second;
            if (originalItems.count(entry.first.value) != 0 || item->pid != PROTO_ID_FLARE) continue;
            if (item->owner == actor) remaining += item_count(actor, item);
            else if (item->owner == nullptr && item->tile == actor->tile && item->elevation == actor->elevation) ++onGround;
        }
        passed = onGround == 2 && remaining == 1 && passed;
        passed = drop(caps, 7, revision) == CommandExecutionStatus::Applied
            && item_caps_get_amount(caps) == 7 && caps->owner == nullptr && passed;
        Object* remainder = session.entities().findObject(lastSplitEntityId);
        passed = remainder != nullptr && item_count(actor, remainder) == 3
            && drop(remainder, 1, revision) == CommandExecutionStatus::Applied
            && item_caps_get_amount(remainder) == 1 && passed;
        armor->flags |= OBJECT_WORN;
        adjust_ac(actor, nullptr, armor);
        passed = stat_get_bonus(actor, STAT_ARMOR_CLASS) == savedBuild.bonusStats[STAT_ARMOR_CLASS] + item_ar_ac(armor)
            && drop(armor, 1, revision) == CommandExecutionStatus::Applied
            && inven_worn(actor) == nullptr
            && player->build.bonusStats == savedBuild.bonusStats && passed;
        passed = actor->data.critter.combat.ap == ap && passed;
        EquipmentCommand close;
        close.turnRevision = revision;
        close.action = EquipmentAction::CloseInventory;
        passed = commandExecutor.setEquipment(actor, close) == CommandExecutionStatus::Applied && passed;
        Object* noAccessItem = session.entities().findObject(lastSplitEntityId);
        if (noAccessItem == nullptr) {
            for (const auto& entry : worldItems) {
                if (originalItems.count(entry.first.value) == 0 && entry.second->owner == actor) {
                    noAccessItem = entry.second;
                    break;
                }
            }
        }
        passed = noAccessItem != nullptr && drop(noAccessItem, 1, revision) == CommandExecutionStatus::InvalidAction && passed;
        // Restore the already-paid open session without another AP charge.
        openInventories[session.playerActorId(kHostPlayerId)] = revision;
    }
    std::vector<EntityId> created;
    for (const auto& entry : worldItems) if (originalItems.count(entry.first.value) == 0) created.push_back(entry.first);
    for (EntityId id : created) {
        Object* item = session.entities().findObject(id);
        if (item == nullptr) continue;
        if (item->owner != nullptr) item_remove_mult(item->owner, item, item_count(item->owner, item));
        obj_erase_object(item, nullptr);
    }
    player->build = savedBuild;
    obj_change_fid(actor, savedFid, nullptr);
    std::fprintf(stderr, "COMBAT_INVENTORY_DROP_%s ap=%d stacks=2 caps=7+1 armor=removed\n",
        passed ? "PASS" : "FAIL", actor->data.critter.combat.ap);
    return passed;
}

bool networkWorldRunPartyRecoverySmokeTest(bool (*processDefeat)())
{
    if (processDefeat == nullptr) return false;
    if (worldMode == NetworkLaunchMode::Join) {
        // Mirror phase revisions without executing any replica queue rules.
        bool passed = true;
        int originalQuit = game_user_wants_to_quit;
        for (PlayerId playerId : session.players().playerIds()) {
            Object* actor = networkWorldPlayerActor(playerId);
            if (actor == nullptr) return false;
            int flags = actor->data.critter.combat.results;
            actor->data.critter.combat.results |= DAM_DEAD;
            for (SessionPhase phase : { SessionPhase::Dialogue, SessionPhase::Transition }) {
                passed = session.transitionTo(phase) == LocalSessionError::None && passed;
                passed = !processDefeat() && game_user_wants_to_quit == originalQuit && passed;
                passed = session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None && passed;
            }
            actor->data.critter.combat.results = flags;
        }
        std::fprintf(stderr, "PARTY_RECOVERY_%s role=guest awaiting_authoritative_ending=checked\n",
            passed ? "PASS" : "FAIL");
        return passed;
    }
    if (worldMode != NetworkLaunchMode::Host) return false;
    if (session.phase() != SessionPhase::Exploration || isInCombat()) return false;
    std::vector<QueueEventState> originalQueue;
    if (!queue_capture_state(originalQueue)) return false;
    int originalTime = game_time();
    int originalQuit = game_user_wants_to_quit;
    WorldMapState originalMap;
    WorldMapTravelProgress originalTravel;
    worldmap_capture_state(originalMap);
    worldmap_capture_travel_progress(originalTravel);
    bool passed = true;
    for (PlayerId playerId : session.players().playerIds()) {
        Object* actor = networkWorldPlayerActor(playerId);
        if (actor == nullptr) { passed = false; break; }
        CritterCombatData originalCombat = actor->data.critter.combat;
        int originalFlags = actor->flags;
        int originalFid = actor->fid;
        int originalHp = actor->data.critter.hp;
        queue_clear();
        actor->data.critter.combat.results = DAM_KNOCKED_OUT;
        passed = !processDefeat() && game_user_wants_to_quit == originalQuit && passed;
        // A blocked vote command must not leave the awake talker waiting for
        // the knockout player's ballot until the normal sixty-second deadline.
        auto roster = session.players().playerIds();
        auto awakePlayer = std::find_if(roster.begin(), roster.end(), [&](PlayerId id) { return id != playerId; });
        if (awakePlayer == roster.end()) return false;
        PlayerId awake = *awakePlayer;
        passed = dialogueVotes.begin(1, awake, kHostPlayerId,
            session.players().playerIds(), 2, DialogueVotingPolicy::MajorityStatsRandomTie, 1000) && passed;
        networkWorldDialogueSetConnected(playerId, true);
        passed = !dialogueVotes.vote(playerId, 1, 1) && passed;
        for (PlayerId voter : roster) {
            if (voter != playerId) passed = dialogueVotes.vote(voter, 1, 0) && passed;
        }
        passed = dialogueVotes.resolve(1) == std::optional<std::uint8_t> { 0 } && passed;
        dialogueVotes.clear();
        passed = queue_add(1, actor, nullptr, EVENT_TYPE_KNOCKOUT) == 0 && passed;
        set_game_time(originalTime + 1);
        queue_process();
        passed = !queue_find(actor, EVENT_TYPE_KNOCKOUT)
            && (actor->data.critter.combat.results & DAM_KNOCKED_OUT) == 0
            && !networkWorldPartyDefeated() && passed;
        register_clear(actor);
        obj_change_fid(actor, originalFid, nullptr);
        actor->data.critter.combat = originalCombat;

        // Dialogue and travel use nested native input loops. The host must
        // unwind them even though the ordinary main loop is not running.
        actor->data.critter.combat.results |= DAM_DEAD;
        passed = session.transitionTo(SessionPhase::Dialogue) == LocalSessionError::None && passed;
        game_user_wants_to_quit = 0;
        passed = processDefeat() && game_user_wants_to_quit == 2 && passed;
        game_user_wants_to_quit = originalQuit;
        passed = session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None && passed;
        passed = session.transitionTo(SessionPhase::Transition) == LocalSessionError::None && passed;
        game_user_wants_to_quit = 0;
        passed = processDefeat() && game_user_wants_to_quit == 2 && passed;
        game_user_wants_to_quit = originalQuit;
        passed = session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None && passed;
        int beforeRest = game_time();
        passed = advanceSharedRest(180, false) && game_time() == beforeRest && passed;
        WorldMapState travelFixture = originalMap;
        travelFixture.x = 1325; travelFixture.y = 325;
        if (worldmap_apply_state(travelFixture) && worldmap_authoritative_travel_begin(1328, 325)) {
            auto step = worldmap_authoritative_travel_step();
            passed = step.status == WorldMapTravelStepStatus::QueueInterrupted
                && game_time() == beforeRest && passed;
        } else {
            passed = false;
        }
        worldmap_authoritative_travel_cancel();
        actor->data.critter.combat = originalCombat;
        // Simulate a scripted lethal outcome whose native queue callback
        // returns zero. Rest/travel must still stop at this exact boundary.
        struct RestoreQueueHandler {
            QueueEventHandler* handler = q_func[EVENT_TYPE_KNOCKOUT].handlerProc;
            ~RestoreQueueHandler() { q_func[EVENT_TYPE_KNOCKOUT].handlerProc = handler; }
        } restoreHandler;
        q_func[EVENT_TYPE_KNOCKOUT].handlerProc = [](Object* owner, void*) {
            owner->data.critter.hp = 0;
            owner->data.critter.combat.results |= DAM_DEAD;
            return 0;
        };
        for (bool travel : { false, true }) {
            queue_clear();
            actor->data.critter.combat = originalCombat;
            actor->data.critter.hp = originalHp;
            set_game_time(originalTime);
            if (queue_add(1, actor, nullptr, EVENT_TYPE_KNOCKOUT) != 0) {
                passed = false;
                continue;
            }
            if (travel) {
                if (worldmap_apply_state(travelFixture) && worldmap_authoritative_travel_begin(1328, 325)) {
                    WorldMapTravelStepResult step;
                    for (int attempt = 0; attempt < 32; ++attempt) {
                        step = worldmap_authoritative_travel_step();
                        if (step.status != WorldMapTravelStepStatus::Moving) break;
                    }
                    bool stoppedAtDeath = step.status == WorldMapTravelStepStatus::QueueInterrupted
                        && game_time() == originalTime + 1;
                    if (!stoppedAtDeath) std::fprintf(stderr,
                        "PARTY_RECOVERY_TRAVEL_FAILED status=%d elapsed=%d defeated=%d\n",
                        static_cast<int>(step.status), game_time() - originalTime, networkWorldPartyDefeated());
                    passed = stoppedAtDeath && passed;
                } else passed = false;
                worldmap_authoritative_travel_cancel();
            } else {
                passed = advanceSharedRest(180, false)
                    && game_time() == originalTime + 1 && passed;
            }
            passed = networkWorldPartyDefeated() && passed;
        }
        actor->data.critter.combat = originalCombat;
        actor->data.critter.hp = originalHp;
        actor->flags = originalFlags;
        set_game_time(originalTime);
    }
    game_user_wants_to_quit = originalQuit;
    passed = queue_replace_state(originalQueue) && passed;
    passed = worldmap_apply_state(originalMap) && passed;
    passed = worldmap_apply_travel_progress(originalTravel) && passed;
    set_game_time(originalTime);
    std::fprintf(stderr, "PARTY_RECOVERY_%s roster=%zu queue_wake=checked incapacitated_ballot=excluded nested_defeat=checked terminal_rest_travel=blocked zero_return_lethal_queue=checked\n",
        passed ? "PASS" : "FAIL", session.players().size());
    return passed;
}

static bool partyFailureSmokeTest()
{
    struct Saved { Object* actor; int flags; int hp; };
    std::vector<Saved> saved;
    for (PlayerId playerId : session.players().playerIds()) {
        Object* actor = networkWorldPlayerActor(playerId);
        if (actor == nullptr) return false;
        saved.push_back({ actor, actor->data.critter.combat.results, actor->data.critter.hp });
    }
    if (saved.size() < 2) return false;
    for (const auto& entry : saved) {
        entry.actor->data.critter.combat.results = 0;
        entry.actor->data.critter.hp = std::max(1, entry.hp);
    }
    bool passed = !networkWorldPartyDefeated();
    Object* nonPlayer = worldCritters.empty() ? nullptr : worldCritters.front().second;
    if (nonPlayer != nullptr) {
        int flags = nonPlayer->data.critter.combat.results;
        nonPlayer->data.critter.combat.results |= DAM_DEAD;
        passed = !networkWorldPartyDefeated() && passed;
        nonPlayer->data.critter.combat.results = flags;
    }
    for (const auto& entry : saved) {
        GameCommand action;
        action.payload = MoveCommand { entry.actor->tile, entry.actor->elevation };
        entry.actor->data.critter.combat.results = DAM_KNOCKED_OUT;
        passed = !networkWorldPartyDefeated()
            && !commandExecutor.actorCanExecute(entry.actor, action) && passed;
        EquipmentCommand close;
        close.action = EquipmentAction::CloseInventory;
        action.payload = close;
        passed = commandExecutor.actorCanExecute(entry.actor, action) && passed;
        action.payload = EndTurnCommand { 1 };
        passed = commandExecutor.actorCanExecute(entry.actor, action) && passed;
        entry.actor->data.critter.combat.results = DAM_DEAD;
        passed = networkWorldPartyDefeated() && passed;
        entry.actor->data.critter.combat.results = 0;
        entry.actor->data.critter.hp = 0;
        passed = networkWorldPartyDefeated() && passed;
        entry.actor->data.critter.hp = std::max(1, entry.hp);
    }
    for (const auto& entry : saved) entry.actor->data.critter.combat.results = DAM_KNOCKED_OUT;
    passed = networkWorldPartyDefeated() && passed;
    for (const auto& entry : saved) {
        entry.actor->data.critter.combat.results = entry.flags;
        entry.actor->data.critter.hp = entry.hp;
    }
    std::fprintf(stderr, "PARTY_FAILURE_%s roster=%zu death=any knockout=all corpse=excluded action_gate=checked\n",
        passed ? "PASS" : "FAIL", saved.size());
    return passed;
}

bool networkWorldRunPlayerRulesSmokeTest()
{
    Object* host = networkWorldPlayerActor(kHostPlayerId);
    Object* guest = networkWorldPlayerActor(kGuestPlayerId);
    auto* hostPlayer = playerStateForActor(host);
    auto* guestPlayer = playerStateForActor(guest);
    Object* target = !worldCritters.empty() ? worldCritters.front().second : nullptr;
    if (worldMode != NetworkLaunchMode::Host || hostPlayer == nullptr
        || guestPlayer == nullptr || target == nullptr || inven_right_hand(guest) == nullptr) return false;
    CharacterBuild savedHost = hostPlayer->build;
    CharacterBuild savedGuest = guestPlayer->build;
    guestPlayer->build.traits = { -1, -1 };
    guestPlayer->build.perkRanks.fill(0);
    hostPlayer->build.baseStats[STAT_ENDURANCE] = 3;
    guestPlayer->build.baseStats[STAT_ENDURANCE] = 8;
    guestPlayer->build.baseStats[STAT_MAXIMUM_HIT_POINTS] = 57;
    guestPlayer->build.bonusStats[STAT_ENDURANCE] = 0;
    guestPlayer->build.bonusStats[STAT_MAXIMUM_HIT_POINTS] = 0;
    bool passed = partyFailureSmokeTest();
    {
        ScopedActingPlayerContext hostContext(*hostPlayer, host);
        passed = stat_get_base_direct(guest, STAT_ENDURANCE) == 8
            && stat_level(guest, STAT_MAXIMUM_HIT_POINTS) == 57 && passed;
        stat_set_bonus(guest, STAT_DAMAGE_RESISTANCE, 17);
        passed = passed && guestPlayer->build.bonusStats[STAT_DAMAGE_RESISTANCE] == 17
            && hostPlayer->build.bonusStats[STAT_DAMAGE_RESISTANCE] == savedHost.bonusStats[STAT_DAMAGE_RESISTANCE]
            && actingPlayerActor() == host;
    }
    {
        ScopedActingPlayerContext hostContext(*hostPlayer, host);
        CharacterBuild queryGuest = guestPlayer->build;
        guestPlayer->build.traits = { TRAIT_GIFTED, -1 };
        guestPlayer->build.baseStats[STAT_STRENGTH] = 6;
        int hostStrength = hostPlayer->build.baseStats[STAT_STRENGTH];
        passed = inc_stat(guest, STAT_STRENGTH) == 0
            && guestPlayer->build.baseStats[STAT_STRENGTH] == 7 && passed;
        passed = dec_stat(guest, STAT_STRENGTH) == 0
            && guestPlayer->build.baseStats[STAT_STRENGTH] == 6
            && hostPlayer->build.baseStats[STAT_STRENGTH] == hostStrength
            && actingPlayerActor() == host && passed;
        guestPlayer->build.traits = { -1, -1 };
        int hostCost = item_w_mp_cost(host, HIT_MODE_RIGHT_WEAPON_PRIMARY, false);
        int guestCost = item_w_mp_cost(guest, HIT_MODE_RIGHT_WEAPON_PRIMARY, false);
        guestPlayer->build.traits = { TRAIT_FAST_SHOT, -1 };
        passed = guestCost > 1
            && item_w_mp_cost(guest, HIT_MODE_RIGHT_WEAPON_PRIMARY, false) == guestCost - 1
            && item_w_called_shot(guest, HIT_MODE_RIGHT_WEAPON_PRIMARY) == 0
            && item_w_mp_cost(host, HIT_MODE_RIGHT_WEAPON_PRIMARY, false) == hostCost
            && actingPlayerActor() == host && passed;
        guestPlayer->build.perkRanks[PERK_SPEAKER] = 1;
        passed = perk_adjust_skill(guest, SKILL_SPEECH) == 20 && actingPlayerActor() == host && passed;
        {
            ScopedActingPlayerContext guestContext(*guestPlayer, guest);
            passed = item_w_mp_cost(host, HIT_MODE_RIGHT_WEAPON_PRIMARY, false) == hostCost
                && actingPlayerActor() == guest && passed;
        }
        Object* grenade = nullptr;
        Object* pistol = inven_right_hand(guest);
        int pistolFlags = pistol->flags;
        if (obj_pid_new(&grenade, 25) == 0 && grenade != nullptr
            && obj_disconnect(grenade, nullptr) == 0
            && item_add_force(guest, grenade, 1) == 0) {
            pistol->flags &= ~OBJECT_IN_RIGHT_HAND;
            grenade->flags |= OBJECT_IN_RIGHT_HAND;
            guestPlayer->build.traits = { -1, -1 };
            guestPlayer->build.baseStats[STAT_STRENGTH] = 1;
            guestPlayer->build.bonusStats[STAT_STRENGTH] = 0;
            int throwRange = item_w_range(guest, HIT_MODE_RIGHT_WEAPON_PRIMARY);
            guestPlayer->build.perkRanks[PERK_HEAVE_HO] = 1;
            passed = throwRange == 3 && item_w_range(guest, HIT_MODE_RIGHT_WEAPON_PRIMARY) == 9
                && actingPlayerActor() == host && passed;
            item_remove_mult(guest, grenade, 1);
            obj_erase_object(grenade, nullptr);
        } else {
            if (grenade != nullptr) obj_erase_object(grenade, nullptr);
            passed = false;
        }
        pistol->flags = pistolFlags;
        guestPlayer->build = queryGuest;
    }
    {
        ScopedActingPlayerContext context(*hostPlayer, host);
        hostPlayer->build.healingSkillUses = {};
        guestPlayer->build.healingSkillUses = {};
        for (int attempt = 0; attempt < 3; ++attempt) passed = skill_use_slot_add(SKILL_FIRST_AID) == 0 && passed;
        passed = skill_use_slot_available(SKILL_FIRST_AID) == -1 && passed;
        {
            ScopedActingPlayerContext guestContext(*guestPlayer, guest);
            passed = skill_use_slot_available(SKILL_FIRST_AID) == 0 && passed;
            passed = skill_use_slot_add(SKILL_DOCTOR) == 0 && passed;
        }
        passed = skill_use_slot_available(SKILL_DOCTOR) == 0 && passed;
    }
    {
        ScopedActingPlayerContext context(*hostPlayer, host);
        guestPlayer->build.unspentSkillPoints = 2;
        int originalPoints = guestPlayer->build.skillPoints[SKILL_SPEECH];
        passed = skill_inc_point(guest, SKILL_SPEECH) == 0 && passed;
        passed = guestPlayer->build.skillPoints[SKILL_SPEECH] == originalPoints + 1
            && guestPlayer->build.unspentSkillPoints == 1 && actingPlayerActor() == host && passed;
        passed = skill_dec_point(guest, SKILL_SPEECH) == 0 && passed;
        passed = guestPlayer->build.skillPoints[SKILL_SPEECH] == originalPoints
            && guestPlayer->build.unspentSkillPoints == 2 && passed;
        hostPlayer->build.addictions = 0;
        guestPlayer->build.addictions = 0;
        item_d_set_addict(PROTO_ID_BUFF_OUT);
        {
            ScopedActingPlayerContext guestContext(*guestPlayer, guest);
            passed = !item_d_check_addict(PROTO_ID_BUFF_OUT) && passed;
            item_d_set_addict(PROTO_ID_BEER);
            passed = item_d_check_addict(PROTO_ID_BOOZE) && passed;
            item_d_unset_addict(PROTO_ID_BOOZE);
            passed = !item_d_check_addict(-1) && !is_pc_flag(PC_FLAG_ADDICTED) && passed;
        }
        passed = item_d_check_addict(PROTO_ID_BUFF_OUT) && is_pc_flag(PC_FLAG_ADDICTED) && passed;
        item_d_unset_addict(PROTO_ID_BUFF_OUT);
        passed = !item_d_check_addict(-1) && passed;
    }
    {
        // Exercise actual native effects under the other player's ambient
        // context, then restore every timer and native value for later tests.
        ScopedActingPlayerContext context(*hostPlayer, host);
        std::vector<QueueEventState> savedQueue;
        bool effectsPassed = queue_capture_state(savedQueue);
        int savedTime = game_time();
        int hostPoison = host->data.critter.poison;
        int guestPoison = guest->data.critter.poison;
        int guestRadiation = guest->data.critter.radiation;
        int hostHp = host->data.critter.hp;
        int guestHp = guest->data.critter.hp;
        int hostFlags = host->flags;
        int guestFlags = guest->flags;
        CharacterBuild beforeHost = hostPlayer->build;
        CharacterBuild beforeGuest = guestPlayer->build;
        effectsPassed = queue_replace_state({}) && effectsPassed;
        host->data.critter.poison = 0;
        guest->data.critter.poison = 0;
        guest->data.critter.radiation = 0;
        guestPlayer->build.bonusStats[STAT_POISON_RESISTANCE] = 0;
        guestPlayer->build.baseStats[STAT_POISON_RESISTANCE] = 50;
        hostPlayer->build.baseStats[STAT_POISON_RESISTANCE] = 0;
        hostPlayer->build.bonusStats[STAT_POISON_RESISTANCE] = 0;
        effectsPassed = critter_adjust_poison(host, 2) == 0
            && critter_adjust_poison(guest, 12) == 0
            && guest->data.critter.poison == 6
            && host->data.critter.poison == 2
            && queue_find(host, EVENT_TYPE_POISON) && queue_find(guest, EVENT_TYPE_POISON)
            && actingPlayerActor() == host && effectsPassed;
        set_game_time(savedTime + 10 * (505 - 5 * 6));
        queue_process();
        effectsPassed = guest->data.critter.poison == 4
            && guest->data.critter.hp == guestHp - 1
            && host->data.critter.hp == hostHp && host->data.critter.poison == 2
            && queue_find(host, EVENT_TYPE_POISON) && queue_find(guest, EVENT_TYPE_POISON)
            && effectsPassed;
        queue_remove_this(host, EVENT_TYPE_POISON);
        queue_remove_this(guest, EVENT_TYPE_POISON);
        guestPlayer->build.baseStats[STAT_RADIATION_RESISTANCE] = 50;
        guestPlayer->build.bonusStats[STAT_RADIATION_RESISTANCE] = 0;
        hostPlayer->build.prototypeFlags &= ~CRITTER_BARTER;
        guestPlayer->build.prototypeFlags &= ~CRITTER_BARTER;
        effectsPassed = critter_adjust_rads(guest, 800) == 0
            && guest->data.critter.radiation == 400
            && (guestPlayer->build.prototypeFlags & CRITTER_BARTER) != 0
            && (hostPlayer->build.prototypeFlags & CRITTER_BARTER) == 0
            && actingPlayerActor() == host && effectsPassed;
        WorldSnapshot toxinsSnapshot;
        bool capturedToxins = networkWorldCaptureSnapshot(EventSequence {}, toxinsSnapshot);
        auto capturedGuest = std::find_if(toxinsSnapshot.actors.begin(), toxinsSnapshot.actors.end(),
            [](const ActorSnapshot& actor) { return actor.ownerId == kGuestPlayerId; });
        effectsPassed = capturedToxins && capturedGuest != toxinsSnapshot.actors.end()
            && capturedGuest->poison == 4 && capturedGuest->radiation == 400 && effectsPassed;
        if (capturedToxins) {
            guest->data.critter.poison = 0;
            guest->data.critter.radiation = 0;
            effectsPassed = applyActorAndCritterState(toxinsSnapshot, false)
                && guest->data.critter.poison == 4 && guest->data.critter.radiation == 400
                && host->data.critter.poison == 2 && effectsPassed;
        }
        // Curing poison must remove the outstanding timer. A stale callback
        // must not damage either player after the numeric effect is gone.
        effectsPassed = queue_add(10, guest, nullptr, EVENT_TYPE_POISON) == 0 && effectsPassed;
        int beforeCureHp = guest->data.critter.hp;
        effectsPassed = critter_adjust_poison(guest, -100) == 0
            && guest->data.critter.poison == 0 && !queue_find(guest, EVENT_TYPE_POISON)
            && critter_check_poison(guest, nullptr) == 0
            && guest->data.critter.hp == beforeCureHp && host->data.critter.hp == hostHp
            && effectsPassed;
        int targetPoison = target->data.critter.poison;
        int targetRadiation = target->data.critter.radiation;
        effectsPassed = critter_adjust_poison(target, 10) == -1
            && critter_adjust_rads(target, 10) == -1
            && target->data.critter.poison == targetPoison
            && target->data.critter.radiation == targetRadiation && effectsPassed;
        RadiationEvent* hostHealing = static_cast<RadiationEvent*>(mem_malloc(sizeof(RadiationEvent)));
        if (hostHealing != nullptr) {
            *hostHealing = { RADIATION_LEVEL_FATAL, 1 };
            effectsPassed = queue_add(GAME_TIME_TICKS_PER_DAY, host, hostHealing, EVENT_TYPE_RADIATION) == 0 && effectsPassed;
            critter_check_rads(guest);
            effectsPassed = queue_find(guest, EVENT_TYPE_RADIATION)
                && queue_find(host, EVENT_TYPE_RADIATION)
                && (guestPlayer->build.prototypeFlags & CRITTER_BARTER) == 0
                && effectsPassed;
            int guestStrength = stat_get_bonus(guest, STAT_STRENGTH);
            int hostStrength = stat_get_bonus(host, STAT_STRENGTH);
            RadiationEvent damage { RADIATION_LEVEL_ADVANCED, 0 };
            critter_process_rads(guest, &damage);
            effectsPassed = stat_get_bonus(guest, STAT_STRENGTH) == guestStrength - 1
                && stat_get_bonus(host, STAT_STRENGTH) == hostStrength
                && queue_find(host, EVENT_TYPE_RADIATION)
                && queue_find(guest, EVENT_TYPE_RADIATION)
                && actingPlayerActor() == host && effectsPassed;
            RadiationEvent healing { RADIATION_LEVEL_ADVANCED, 1 };
            critter_process_rads(guest, &healing);
            effectsPassed = stat_get_bonus(guest, STAT_STRENGTH) == guestStrength
                && stat_get_bonus(host, STAT_STRENGTH) == hostStrength
                && queue_find(host, EVENT_TYPE_RADIATION) && effectsPassed;
        } else effectsPassed = false;
        set_game_time(savedTime);
        effectsPassed = queue_replace_state(savedQueue) && effectsPassed;
        hostPlayer->build = beforeHost;
        guestPlayer->build = beforeGuest;
        host->data.critter.poison = hostPoison;
        guest->data.critter.poison = guestPoison;
        guest->data.critter.radiation = guestRadiation;
        host->data.critter.hp = hostHp;
        guest->data.critter.hp = guestHp;
        host->flags = hostFlags;
        guest->flags = guestFlags;
        std::fprintf(stderr, "NATIVE_PLAYER_TOXINS_%s poison_tick=1 cure_timer=1 stale_tick=1 radiation_damage=1 radiation_heal=1 owner_isolation=1 numeric_replica=1 npc_unchanged=1\n",
            effectsPassed ? "PASS" : "FAIL");
        passed = effectsPassed && passed;
    }
    guestPlayer->build.skillPoints[SKILL_SMALL_GUNS] = 0;
    int baseline = determine_to_hit_no_range(guest, target, HIT_LOCATION_TORSO, HIT_MODE_RIGHT_WEAPON_PRIMARY);
    guestPlayer->build.traits[0] = TRAIT_ONE_HANDER;
    int adjusted = determine_to_hit_no_range(guest, target, HIT_LOCATION_TORSO, HIT_MODE_RIGHT_WEAPON_PRIMARY);
    passed = passed && !item_w_is_2handed(inven_right_hand(guest)) && baseline < 75 && adjusted == baseline + 20;
    hostPlayer->build = savedHost;
    guestPlayer->build = savedGuest;
    std::fprintf(stderr, "PLAYER_RULES_SMOKE_%s guest_stat=8 guest_hp=57 accuracy=%d one_hander=%d\n",
        passed ? "PASS" : "FAIL", baseline, adjusted);
    return passed;
}

bool networkWorldPrepareEquipmentSmoke(int weaponPid, bool ownedContainer)
{
    if (worldMode != NetworkLaunchMode::Host) return false;
    for (PlayerId playerId : { kHostPlayerId, kGuestPlayerId }) {
        Object* actor = networkWorldPlayerActor(playerId);
        if (actor == nullptr) return false;
        Object* weapon = inven_pid_is_carried_ptr(actor, weaponPid);
        if (weapon == nullptr) {
            if (obj_pid_new(&weapon, weaponPid) == -1 || weapon == nullptr) return false;
            if (obj_disconnect(weapon, nullptr) == -1
                || item_add_force(actor, weapon, 1) != 0 || !registerItem(weapon)) return false;
        }
        if (weaponPid == 4 && playerId == kHostPlayerId && item_count(actor, weapon) == 1) {
            // A free knife trade leaves the recipient with a stack. Equipping
            // one must preserve the other knife and both shared identities.
            Object* extra = nullptr;
            if (obj_pid_new(&extra, weaponPid) == -1 || extra == nullptr
                || obj_disconnect(extra, nullptr) == -1
                || item_add_force(actor, extra, 1) != 0) return false;
            weapon = inven_pid_is_carried_ptr(actor, weaponPid);
        }
        if (ownedContainer) {
            Object* bag = nullptr;
            if (obj_pid_new(&bag, 211) != 0 || bag == nullptr
                || obj_disconnect(bag, nullptr) == -1
                || item_add_force(actor, bag, 1) != 0 || !registerItem(bag)) return false;
        }
        if (weapon == nullptr || item_count(actor, weapon) < 1) return false;
        item_w_set_curr_ammo(weapon, item_w_max_ammo(weapon));
    }
    return true;
}

bool networkWorldPrepareCombatReloadSmoke()
{
    Object* guest = networkWorldPlayerActor(kGuestPlayerId);
    if (worldMode != NetworkLaunchMode::Host || guest == nullptr) return false;
    int weaponPid = -1;
    int ammoPid = -1;
    for (int pid = 1; pid < 200 && weaponPid < 0; pid++) {
        Proto* weaponProto = nullptr;
        if (proto_ptr(pid, &weaponProto) != 0 || weaponProto == nullptr
            || weaponProto->item.type != ITEM_TYPE_WEAPON
            || weaponProto->item.data.weapon.ammoCapacity <= 0) continue;
        int candidateAmmo = weaponProto->item.data.weapon.ammoTypePid;
        Proto* ammoProto = nullptr;
        if (candidateAmmo <= 0 || proto_ptr(candidateAmmo, &ammoProto) != 0
            || ammoProto == nullptr || ammoProto->item.type != ITEM_TYPE_AMMO
            || ammoProto->item.data.ammo.caliber
                != weaponProto->item.data.weapon.caliber) continue;
        weaponPid = pid;
        ammoPid = candidateAmmo;
    }
    if (weaponPid < 0) return false;
    auto ensureItem = [&](int pid) -> Object* {
        Object* item = inven_pid_is_carried_ptr(guest, pid);
        if (item != nullptr) return item;
        if (obj_pid_new(&item, pid) == -1 || item == nullptr) return nullptr;
        if (obj_disconnect(item, nullptr) == -1 || item_add_force(guest, item, 1) != 0) {
            obj_erase_object(item, nullptr);
            return nullptr;
        }
        return item;
    };
    Object* weapon = ensureItem(weaponPid);
    Object* ammo = ensureItem(ammoPid);
    if (weapon == nullptr || ammo == nullptr) return false;
    weapon->flags |= OBJECT_IN_RIGHT_HAND;
    item_w_set_curr_ammo(weapon, 0);
    return static_cast<bool>(registerItem(weapon))
        && static_cast<bool>(registerItem(ammo));
}

bool networkWorldPrepareCombatSeparatedElevationSmoke()
{
    Object* guest = networkWorldPlayerActor(kGuestPlayerId);
    if (worldMode != NetworkLaunchMode::Host || guest == nullptr
        || guest->elevation != 0) return false;
    return obj_move_to_tile(guest, guest->tile, 1, nullptr) == 0
        && guest->elevation == 1;
}

bool networkWorldPrepareCombatScriptStatusSmoke()
{
    Object* guest = networkWorldPlayerActor(kGuestPlayerId);
    Object* target = !worldCritters.empty() ? worldCritters.front().second : nullptr;
    if (worldMode != NetworkLaunchMode::Host || guest == nullptr
        || target == nullptr) return false;
    guest->data.critter.combat.results |= DAM_KNOCKED_OUT;
    target->data.critter.combat.maneuver |= CRITTER_MANUEVER_FLEEING;
    return true;
}

bool networkWorldCombatScriptStatusObserved()
{
    Object* guest = networkWorldPlayerActor(kGuestPlayerId);
    Object* target = !worldCritters.empty() ? worldCritters.front().second : nullptr;
    return guest != nullptr && target != nullptr
        && (guest->data.critter.combat.results & DAM_KNOCKED_OUT) != 0
        && (target->data.critter.combat.maneuver & CRITTER_MANUEVER_FLEEING) != 0;
}

bool networkWorldPrepareCombatAttackSmoke(bool lethal)
{
    if (worldMode != NetworkLaunchMode::Host || worldCritters.empty()) {
        return false;
    }
    // Native UI testing may leave a fidget registered while its synthetic
    // fixture only pumps networking. Settle it before placing the combatants
    // and capturing their authoritative starting state.
    anim_stop();
    Object* guest = networkWorldPlayerActor(kGuestPlayerId);
    Object* target = worldCritters.front().second;
    if (guest == nullptr || target == nullptr || guest == target
        || (target->data.critter.combat.results & DAM_DEAD) != 0) {
        return false;
    }
    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        int tile = tile_num_in_direction(guest->tile, rotation, 1);
        if (tile >= 0
            && obj_blocking_at(target, tile, guest->elevation) == nullptr
            && obj_move_to_tile(target, tile, guest->elevation, nullptr) == 0) {
            if (lethal) {
                target->data.critter.hp = 1;
                ProgramValue value;
                // Rat scripts allocate locals lazily. The original fixture
                // killed before that happened and missed death compaction.
                if (target->sid == -1
                    || scr_get_local_var(target->sid, 0, value) == -1
                    || num_map_local_vars == 0) return false;
            }
            return obj_dist(guest, target) == 1;
        }
    }
    return false;
}

std::optional<EntityId> networkWorldPrepareDoorSmokeTest()
{
    if (!session.isActive() || worldDoors.empty()) {
        return std::nullopt;
    }
    Object* actor = session.entities().findObject(session.playerActorId(kGuestPlayerId));
    Object* door = worldDoors.front().second;
    if (actor == nullptr || door == nullptr) {
        return std::nullopt;
    }

    anim_stop();
    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        int tile = tile_num_in_direction(door->tile, rotation, 1);
        if (obj_blocking_at(actor, tile, door->elevation) == nullptr
            && obj_move_to_tile(actor, tile, door->elevation, nullptr) == 0) {
            return worldDoors.front().first;
        }
    }
    return std::nullopt;
}

std::optional<EntityId> networkWorldPreparePickupSmokeTest()
{
    if (!session.isActive()) {
        return std::nullopt;
    }
    Object* actor = session.entities().findObject(session.playerActorId(kGuestPlayerId));
    if (actor == nullptr) {
        return std::nullopt;
    }

    anim_stop();
    for (const auto& entry : worldItems) {
        Object* item = entry.second;
        if (item == nullptr
            || item->owner != nullptr
            || !hexGridTileIsValid(item->tile)
            || !elevationIsValid(item->elevation)
            || item_get_type(item) == ITEM_TYPE_CONTAINER) {
            continue;
        }
        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            int tile = tile_num_in_direction(item->tile, rotation, 1);
            if (hexGridTileIsValid(tile)
                && obj_blocking_at(actor, tile, item->elevation) == nullptr
                && obj_move_to_tile(actor, tile, item->elevation, nullptr) == 0) {
                return entry.first;
            }
        }
    }

    Object* item = nullptr;
    if (obj_pid_new(&item, PROTO_ID_STIMPACK) == -1 || item == nullptr) {
        return std::nullopt;
    }
    // obj_pid_new inserts a new object into Fallout's floating-object list.
    // Remove that node before connecting the fixture to a map tile, otherwise
    // the same object would be owned by two object-list nodes after pickup.
    if (obj_disconnect(item, nullptr) == -1) {
        obj_erase_object(item, nullptr);
        return std::nullopt;
    }
    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        int tile = tile_num_in_direction(actor->tile, rotation, 1);
        if (!hexGridTileIsValid(tile)
            || obj_blocking_at(actor, tile, actor->elevation) != nullptr) {
            continue;
        }
        if (obj_connect(item, tile, actor->elevation, nullptr) == 0) {
            EntityRegistrationResult registration = registerItem(item);
            if (registration) {
                return registration.entityId;
            }
            obj_disconnect(item, nullptr);
        }
        break;
    }
    obj_connect(item, actor->tile, actor->elevation, nullptr);
    obj_erase_object(item, nullptr);
    return std::nullopt;
}

std::optional<EntityId> networkWorldPrepareLootSmokeTest()
{
    if (!session.isActive()) {
        return std::nullopt;
    }
    Object* actor = session.entities().findObject(session.playerActorId(kGuestPlayerId));
    if (actor == nullptr) {
        return std::nullopt;
    }

    anim_stop();
    for (const auto& entry : worldCritters) {
        Object* critter = entry.second;
        if (critter == nullptr
            || critter == actor
            || !hexGridTileIsValid(critter->tile)
            || !elevationIsValid(critter->elevation)) {
            continue;
        }
        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            int tile = tile_num_in_direction(critter->tile, rotation, 1);
            if (hexGridTileIsValid(tile)
                && obj_blocking_at(actor, tile, critter->elevation) == nullptr
                && obj_move_to_tile(actor, tile, critter->elevation, nullptr) == 0) {
                // This fixture represents corpse loot, not unchecked theft
                // from a living NPC. Prepare the same corpse on both peers.
                critter->data.critter.combat.results |= DAM_DEAD;
                critter->data.critter.hp = 0;
                return entry.first;
            }
        }
    }
    return std::nullopt;
}

std::optional<EntityId> networkWorldPrepareSkillSmokeTest()
{
    if (!session.isActive() || worldDoors.empty()) {
        return std::nullopt;
    }
    Object* actor = session.entities().findObject(session.playerActorId(kGuestPlayerId));
    if (actor == nullptr) {
        return std::nullopt;
    }

    anim_stop();
    for (const auto& entry : worldDoors) {
        Object* door = entry.second;
        if (door == nullptr || !hexGridTileIsValid(door->tile) || !elevationIsValid(door->elevation)) {
            continue;
        }
        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            int tile = tile_num_in_direction(door->tile, rotation, 1);
            if (hexGridTileIsValid(tile)
                && obj_blocking_at(actor, tile, door->elevation) == nullptr
                && obj_move_to_tile(actor, tile, door->elevation, nullptr) == 0) {
                return entry.first;
            }
        }
    }
    return std::nullopt;
}

std::optional<EntityId> networkWorldPrepareScenerySmokeTest()
{
    if (!session.isActive() || worldScenery.empty()) {
        return std::nullopt;
    }
    Object* actor = session.entities().findObject(session.playerActorId(kGuestPlayerId));
    if (actor == nullptr) {
        return std::nullopt;
    }

    anim_stop();
    std::vector<std::pair<EntityId, Object*>> candidates = worldScenery;
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        int leftSid = -1;
        int rightSid = -1;
        bool leftScripted = obj_sid(left.second, &leftSid) != -1;
        bool rightScripted = obj_sid(right.second, &rightSid) != -1;
        return leftScripted && !rightScripted;
    });
    for (const auto& entry : candidates) {
        Object* scenery = entry.second;
        if (scenery == nullptr
            || !hexGridTileIsValid(scenery->tile)
            || !elevationIsValid(scenery->elevation)) {
            continue;
        }
        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            int tile = tile_num_in_direction(scenery->tile, rotation, 1);
            if (hexGridTileIsValid(tile)
                && obj_blocking_at(actor, tile, scenery->elevation) == nullptr
                && obj_move_to_tile(actor, tile, scenery->elevation, nullptr) == 0) {
                return entry.first;
            }
        }
    }
    return std::nullopt;
}

bool networkWorldMutateScenerySmokeTest(EntityId targetId)
{
    auto entry = std::find_if(worldScenery.begin(), worldScenery.end(), [&](const auto& candidate) {
        return candidate.first == targetId;
    });
    if (worldMode != NetworkLaunchMode::Host
        || entry == worldScenery.end()
        || entry->second == nullptr) {
        return false;
    }
    entry->second->flags ^= OBJECT_NO_HIGHLIGHT;
    return true;
}

std::optional<EntityId> networkWorldPrepareContainerSmokeTest()
{
    if (!session.isActive()) {
        return std::nullopt;
    }
    Object* actor = session.entities().findObject(session.playerActorId(kGuestPlayerId));
    if (actor == nullptr) {
        return std::nullopt;
    }

    anim_stop();
    for (const auto& entry : worldItems) {
        Object* container = entry.second;
        if (container == nullptr
            || container->owner != nullptr
            || item_get_type(container) != ITEM_TYPE_CONTAINER
            || !hexGridTileIsValid(container->tile)
            || !elevationIsValid(container->elevation)) {
            continue;
        }
        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            int tile = tile_num_in_direction(container->tile, rotation, 1);
            if (hexGridTileIsValid(tile)
                && obj_blocking_at(actor, tile, container->elevation) == nullptr
                && obj_move_to_tile(actor, tile, container->elevation, nullptr) == 0) {
                return entry.first;
            }
        }
    }
    return std::nullopt;
}

bool networkWorldMutateContainerSmokeTest(EntityId targetId)
{
    auto entry = std::find_if(worldItems.begin(), worldItems.end(), [&](const auto& candidate) {
        return candidate.first == targetId;
    });
    if (worldMode != NetworkLaunchMode::Host
        || entry == worldItems.end()
        || entry->second == nullptr
        || item_get_type(entry->second) != ITEM_TYPE_CONTAINER) {
        return false;
    }
    entry->second->flags ^= OBJECT_NO_HIGHLIGHT;
    return true;
}

std::optional<QuestSmokeFixture> networkWorldPrepareQuestSmokeTest()
{
    if (!session.isActive()) {
        return std::nullopt;
    }
    Object* actor = session.entities().findObject(session.playerActorId(kGuestPlayerId));
    PlayerCharacterState* host = session.players().find(kHostPlayerId);
    PlayerCharacterState* guest = session.players().find(kGuestPlayerId);
    if (actor == nullptr || host == nullptr || guest == nullptr) {
        return std::nullopt;
    }

    anim_stop();
    for (const auto& entry : worldCritters) {
        Object* target = entry.second;
        int sid = -1;
        Script* script = nullptr;
        ProgramValue cured;
        if (target == nullptr
            || obj_sid(target, &sid) == -1
            || scr_ptr(sid, &script) == -1
            || script == nullptr
            || script->scr_script_idx != kJarvisScriptIndex
            || scr_get_local_var(sid, kJarvisCuredLocalVariable, cured) == -1
            || cured.integerValue != 0
            || !hexGridTileIsValid(target->tile)
            || !elevationIsValid(target->elevation)) {
            continue;
        }

        bool placed = false;
        for (int rotation = 0; rotation < ROTATION_COUNT && !placed; rotation++) {
            int tile = tile_num_in_direction(target->tile, rotation, 1);
            placed = hexGridTileIsValid(tile)
                && obj_blocking_at(actor, tile, target->elevation) == nullptr
                && obj_move_to_tile(actor, tile, target->elevation, nullptr) == 0;
        }
        if (!placed) {
            return std::nullopt;
        }

        Object* antidote = nullptr;
        if (obj_pid_new(&antidote, kAntidotePid) == -1
            || antidote == nullptr
            || item_add_force(actor, antidote, 1) != 0
            || obj_disconnect(antidote, nullptr) == -1) {
            if (antidote != nullptr) {
                obj_destroy(antidote);
            }
            return std::nullopt;
        }
        EntityRegistrationResult registration = registerItem(antidote);
        if (!registration) {
            obj_destroy(antidote);
            return std::nullopt;
        }
        questSmokeInitialReputation = game_get_global_var(GVAR_PLAYER_REPUATION);
        return QuestSmokeFixture { entry.first, registration.entityId };
    }
    return std::nullopt;
}

bool networkWorldVerifyQuestSmokeTest(const QuestSmokeFixture& fixture)
{
    Object* target = session.entities().findObject(fixture.targetId);
    PlayerCharacterState* host = session.players().find(kHostPlayerId);
    PlayerCharacterState* guest = session.players().find(kGuestPlayerId);
    int sid = -1;
    Script* script = nullptr;
    ProgramValue cured;
    return session.isActive()
        && target != nullptr
        && host != nullptr
        && guest != nullptr
        && obj_sid(target, &sid) != -1
        && scr_ptr(sid, &script) != -1
        && script != nullptr
        && script->scr_script_idx == kJarvisScriptIndex
        && scr_get_local_var(sid, kJarvisCuredLocalVariable, cured) != -1
        && cured.integerValue == 1
        && session.entities().findObject(fixture.itemId) == nullptr
        && networkWorldFindObject(fixture.itemId) == nullptr
        && host->build.experience == 525
        && guest->build.experience == 525
        && game_get_global_var(GVAR_PLAYER_REPUATION) == questSmokeInitialReputation + 1;
}

std::optional<ElevatorSmokeFixture> networkWorldPrepareElevatorSmokeTest()
{
    if (!session.isActive()) {
        return std::nullopt;
    }
    Object* host = session.entities().findObject(session.playerActorId(kHostPlayerId));
    Object* guest = session.entities().findObject(session.playerActorId(kGuestPlayerId));
    if (host == nullptr || guest == nullptr) {
        return std::nullopt;
    }

    anim_stop();
    for (int elevatorType = 0; elevatorType < ELEVATOR_COUNT; elevatorType++) {
        int sourceTile = -1;
        if (!elevator_get_source(elevatorType, map_data.field_34, map_elevation, &sourceTile)
            || !hexGridTileIsValid(sourceTile)) {
            continue;
        }

        int destinationLevel = -1;
        int destinationElevation = -1;
        for (int level = 0; level < elevator_get_level_count(elevatorType); level++) {
            int destinationMap = -1;
            int candidateElevation = -1;
            int destinationTile = -1;
            if (elevator_get_destination(elevatorType,
                    level,
                    &destinationMap,
                    &candidateElevation,
                    &destinationTile)
                && destinationMap == map_data.field_34
                && candidateElevation != map_elevation
                && elevationIsValid(candidateElevation)
                && hexGridTileIsValid(destinationTile)) {
                destinationLevel = level;
                destinationElevation = candidateElevation;
                break;
            }
        }
        if (destinationLevel == -1) {
            continue;
        }

        bool hostPlaced = false;
        for (int distance = 5; distance <= 8 && !hostPlaced; distance++) {
            for (int rotation = 0; rotation < ROTATION_COUNT && !hostPlaced; rotation++) {
                int tile = tile_num_in_direction(sourceTile, rotation, distance);
                hostPlaced = hexGridTileIsValid(tile)
                    && obj_blocking_at(host, tile, map_elevation) == nullptr
                    && obj_move_to_tile(host, tile, map_elevation, nullptr) == 0;
            }
        }
        bool guestPlaced = false;
        for (int rotation = 0; rotation < ROTATION_COUNT && !guestPlaced; rotation++) {
            int tile = tile_num_in_direction(sourceTile, rotation, 1);
            guestPlaced = hexGridTileIsValid(tile)
                && obj_blocking_at(guest, tile, map_elevation) == nullptr
                && obj_move_to_tile(guest, tile, map_elevation, nullptr) == 0;
        }
        if (!hostPlaced || !guestPlaced) {
            return std::nullopt;
        }
        return ElevatorSmokeFixture {
            elevatorType,
            destinationLevel,
            map_elevation,
            host->tile,
            destinationElevation,
        };
    }
    return std::nullopt;
}

bool networkWorldVerifyElevatorSmokeTest(const ElevatorSmokeFixture& fixture)
{
    Object* host = session.entities().findObject(session.playerActorId(kHostPlayerId));
    Object* guest = session.entities().findObject(session.playerActorId(kGuestPlayerId));
    return host != nullptr
        && guest != nullptr
        && session.phase() == SessionPhase::Exploration
        && host->elevation == fixture.sourceElevation
        && host->tile == fixture.hostTile
        && guest->elevation == fixture.destinationElevation
        && map_elevation == (worldMode == NetworkLaunchMode::Host
                ? fixture.sourceElevation
                : fixture.destinationElevation);
}

std::optional<MapTransitionSmokeFixture> networkWorldPrepareMapTransitionSmokeTest()
{
    if (!session.isActive()) {
        return std::nullopt;
    }
    Object* host = session.entities().findObject(session.playerActorId(kHostPlayerId));
    Object* guest = session.entities().findObject(session.playerActorId(kGuestPlayerId));
    if (host == nullptr || guest == nullptr) {
        return std::nullopt;
    }

    anim_stop();
    for (int elevatorType = 0; elevatorType < ELEVATOR_COUNT; elevatorType++) {
        int sourceTile = -1;
        if (!elevator_get_source(elevatorType, map_data.field_34, map_elevation, &sourceTile)
            || !hexGridTileIsValid(sourceTile)) {
            continue;
        }

        int destinationLevel = -1;
        int destinationMap = -1;
        int destinationElevation = -1;
        for (int level = 0; level < elevator_get_level_count(elevatorType); level++) {
            int candidateMap = -1;
            int candidateElevation = -1;
            int candidateTile = -1;
            if (elevator_get_destination(elevatorType,
                    level,
                    &candidateMap,
                    &candidateElevation,
                    &candidateTile)
                && candidateMap != map_data.field_34
                && elevationIsValid(candidateElevation)
                && hexGridTileIsValid(candidateTile)) {
                destinationLevel = level;
                destinationMap = candidateMap;
                destinationElevation = candidateElevation;
                break;
            }
        }
        if (destinationLevel == -1) {
            continue;
        }

        bool guestPlaced = false;
        for (int rotation = 0; rotation < ROTATION_COUNT && !guestPlaced; rotation++) {
            int tile = tile_num_in_direction(sourceTile, rotation, 1);
            guestPlaced = hexGridTileIsValid(tile)
                && obj_blocking_at(guest, tile, map_elevation) == nullptr
                && obj_move_to_tile(guest, tile, map_elevation, nullptr) == 0;
        }
        bool hostRemote = false;
        for (int distance = 5; distance <= 8 && !hostRemote; distance++) {
            for (int rotation = 0; rotation < ROTATION_COUNT && !hostRemote; rotation++) {
                int tile = tile_num_in_direction(sourceTile, rotation, distance);
                hostRemote = hexGridTileIsValid(tile)
                    && obj_blocking_at(host, tile, map_elevation) == nullptr
                    && obj_move_to_tile(host, tile, map_elevation, nullptr) == 0;
            }
        }
        GameCommand readinessProbe;
        readinessProbe.sequence = CommandSequence { 1 };
        readinessProbe.playerId = kGuestPlayerId;
        readinessProbe.actorId = session.playerActorId(kGuestPlayerId);
        readinessProbe.expectedPhase = SessionPhase::Exploration;
        readinessProbe.expectedPhaseRevision = session.phaseRevision();
        readinessProbe.payload = ElevatorCommand { elevatorType, destinationLevel };
        AuthoritativeCommandResult readinessOutcome = guestPlaced && hostRemote
            ? networkWorldProcessCommand(readinessProbe)
            : AuthoritativeCommandResult {};
        bool readinessRejected = readinessOutcome.result.status == CommandStatus::Rejected
            && readinessOutcome.result.rejection == CommandRejection::InvalidAction
            && !readinessOutcome.event.has_value()
            && map_data.field_34 != destinationMap;
        commandProcessor.reset();

        bool hostPlaced = false;
        for (int rotation = 0; rotation < ROTATION_COUNT && !hostPlaced; rotation++) {
            int tile = tile_num_in_direction(sourceTile, rotation, 1);
            hostPlaced = hexGridTileIsValid(tile)
                && obj_blocking_at(host, tile, map_elevation) == nullptr
                && obj_move_to_tile(host, tile, map_elevation, nullptr) == 0;
        }
        int existingGuestCaps = item_caps_total(guest);
        if (!readinessRejected
            || !hostPlaced
            || !guestPlaced
            || (existingGuestCaps > 0 && item_caps_adjust(guest, -existingGuestCaps) != 0)
            || item_caps_adjust(guest, 7) != 0) {
            return std::nullopt;
        }
        Inventory& inventory = guest->data.inventory;
        bool capsRegistered = false;
        for (int index = 0; index < inventory.length; index++) {
            Object* item = inventory.items[index].item;
            if (item != nullptr && item->pid == PROTO_ID_MONEY && inventory.items[index].quantity == 7) {
                capsRegistered = static_cast<bool>(registerItem(item));
                break;
            }
        }
        if (!capsRegistered) {
            return std::nullopt;
        }
        MapTransitionSmokeFixture fixture {
            elevatorType,
            destinationLevel,
            destinationMap,
            destinationElevation,
            7,
            session.playerActorId(kHostPlayerId),
            session.playerActorId(kGuestPlayerId),
        };
        if (worldMode == NetworkLaunchMode::Host) {
            fixture.peerTimerTime = game_time() + 1000;
            fixture.peerTimerAgility = stat_get_bonus(guest, STAT_AGILITY);
            perk_add_effect(guest, PERK_BUFFOUT_ADDICTION);
            guest->data.critter.combat.results |= DAM_KNOCKED_DOWN;
            for (int index = 0; index < guest->data.inventory.length; ++index) {
                const auto& entry = guest->data.inventory.items[index];
                if (entry.item->pid == 79) fixture.peerFlareCount += entry.quantity;
            }
            auto* drug = static_cast<DrugEffectEvent*>(mem_malloc(sizeof(DrugEffectEvent)));
            auto* withdrawal = static_cast<WithdrawalEvent*>(mem_malloc(sizeof(WithdrawalEvent)));
            auto* radiation = static_cast<RadiationEvent*>(mem_malloc(sizeof(RadiationEvent)));
            if (drug == nullptr || withdrawal == nullptr || radiation == nullptr) {
                mem_free(drug); mem_free(withdrawal); mem_free(radiation);
                return std::nullopt;
            }
            *drug = DrugEffectEvent { PROTO_ID_BUFF_OUT, { STAT_AGILITY, -1, -1 }, { 1, 0, 0 } };
            *withdrawal = WithdrawalEvent { 0, PROTO_ID_BUFF_OUT, PERK_BUFFOUT_ADDICTION };
            *radiation = RadiationEvent { 1, 1 };
            if (queue_add(1000, guest, drug, EVENT_TYPE_DRUG) != 0
                || queue_add(1000, guest, withdrawal, EVENT_TYPE_WITHDRAWAL) != 0
                || queue_add(1000, guest, radiation, EVENT_TYPE_RADIATION) != 0
                || queue_add(1000, guest, nullptr, EVENT_TYPE_POISON) != 0
                || queue_add(1000, guest, nullptr, EVENT_TYPE_KNOCKOUT) != 0) return std::nullopt;
            Object* flare = nullptr;
            if (obj_pid_new(&flare, 79) != 0 || flare == nullptr) return std::nullopt;
            flare->flags |= OBJECT_USED;
            if (item_add_force(guest, flare, 1) != 0 || !registerItem(flare)
                || queue_add(1000, flare, nullptr, EVENT_TYPE_FLARE) != 0) return std::nullopt;
        }
        return fixture;
    }
    return std::nullopt;
}

bool networkWorldVerifyMapTransitionSmokeTest(const MapTransitionSmokeFixture& fixture)
{
    Object* host = session.entities().findObject(fixture.hostActorId);
    Object* guest = session.entities().findObject(fixture.guestActorId);
    if (fixture.peerTimerTime != 0 && guest != nullptr
        && map_data.field_34 == fixture.destinationMap && session.phase() == SessionPhase::Exploration) {
        if (queue_find(guest, EVENT_TYPE_DRUG)) {
            if ((guest->data.critter.combat.results & (DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN)) != 0) return false;
            std::vector<QueueEventState> state;
            if (!queue_capture_state(state)) return false;
            for (int type : { EVENT_TYPE_DRUG, EVENT_TYPE_WITHDRAWAL, EVENT_TYPE_POISON, EVENT_TYPE_RADIATION, EVENT_TYPE_KNOCKOUT }) {
                auto event = std::find_if(state.begin(), state.end(), [&](const auto& entry) {
                    return entry.owner == guest && entry.eventType == type;
                });
                if (event == state.end() || event->time != fixture.peerTimerTime) {
                    std::fprintf(stderr, "NATIVE_MAP_PEER_TIMERS_FAIL missing_type=%d\n", type);
                    return false;
                }
            }
            int flareCount = 0;
            for (int index = 0; index < guest->data.inventory.length; ++index) {
                const auto& entry = guest->data.inventory.items[index];
                if (entry.item->pid == 79) flareCount += entry.quantity;
            }
            if (flareCount != fixture.peerFlareCount) return false;
            int savedTime = game_time();
            set_game_time(fixture.peerTimerTime);
            for (int attempt = 0; attempt < 8; ++attempt) {
                queue_process();
                if (!queue_find(guest, EVENT_TYPE_DRUG) && !queue_find(guest, EVENT_TYPE_WITHDRAWAL)
                    && !queue_find(guest, EVENT_TYPE_POISON) && !queue_find(guest, EVENT_TYPE_RADIATION)
                    && !queue_find(guest, EVENT_TYPE_KNOCKOUT)) break;
            }
            set_game_time(savedTime);
            bool processed = stat_get_bonus(guest, STAT_AGILITY) == fixture.peerTimerAgility + 1;
            for (int type : { EVENT_TYPE_DRUG, EVENT_TYPE_WITHDRAWAL, EVENT_TYPE_POISON, EVENT_TYPE_RADIATION, EVENT_TYPE_KNOCKOUT })
                processed = processed && !queue_find(guest, type);
            std::fprintf(stderr, "NATIVE_MAP_PEER_TIMERS_%s owner_rebound=1 absolute_time=1 native_item_destroy=1 native_wake=1 handlers_processed=%d agility=%d expected=%d pending=%d/%d/%d/%d/%d\n",
                processed ? "PASS" : "FAIL", processed, stat_get_bonus(guest, STAT_AGILITY), fixture.peerTimerAgility + 1,
                queue_find(guest, EVENT_TYPE_DRUG), queue_find(guest, EVENT_TYPE_WITHDRAWAL), queue_find(guest, EVENT_TYPE_POISON),
                queue_find(guest, EVENT_TYPE_RADIATION), queue_find(guest, EVENT_TYPE_KNOCKOUT));
            if (!processed) return false;
        } else if (stat_get_bonus(guest, STAT_AGILITY) != fixture.peerTimerAgility + 1) {
            return false;
        }
    }
    bool capsRegistered = false;
    if (guest != nullptr) {
        Inventory& inventory = guest->data.inventory;
        for (int index = 0; index < inventory.length; index++) {
            Object* item = inventory.items[index].item;
            std::optional<EntityId> itemId = item != nullptr
                ? session.entities().findEntity(item)
                : std::nullopt;
            if (item != nullptr
                && item->pid == PROTO_ID_MONEY
                && inventory.items[index].quantity == fixture.guestCaps
                && itemId.has_value()
                && std::any_of(worldItems.begin(), worldItems.end(), [&](const auto& entry) {
                    return entry.first == *itemId && entry.second == item;
                })) {
                capsRegistered = true;
                break;
            }
        }
    }
    return session.isActive()
        && session.playerActorId(kHostPlayerId) == fixture.hostActorId
        && session.playerActorId(kGuestPlayerId) == fixture.guestActorId
        && host != nullptr
        && guest != nullptr
        && host != guest
        && map_data.field_34 == fixture.destinationMap
        && session.phase() == SessionPhase::Exploration
        && host->elevation == fixture.destinationElevation
        && guest->elevation == fixture.destinationElevation
        && host->tile != guest->tile
        && capsRegistered
        && item_caps_total(guest) == fixture.guestCaps
        && map_elevation == fixture.destinationElevation;
}

std::optional<ExitGridSmokeFixture> networkWorldPrepareExitGridSmokeTest()
{
    if (!session.isActive()) {
        return std::nullopt;
    }
    Object* host = session.entities().findObject(session.playerActorId(kHostPlayerId));
    Object* guest = session.entities().findObject(session.playerActorId(kGuestPlayerId));
    if (host == nullptr || guest == nullptr) {
        return std::nullopt;
    }

    anim_stop();
    for (const auto& entry : worldExitGrids) {
        Object* exitGrid = entry.second;
        int destinationMap = -1;
        int destinationTile = -1;
        int destinationElevation = -1;
        int destinationRotation = -1;
        if (!exitGridDestination(exitGrid,
                destinationMap,
                destinationTile,
                destinationElevation,
                destinationRotation)
            || destinationMap == map_data.field_34
            || obj_move_to_tile(guest, exitGrid->tile, exitGrid->elevation, nullptr) == -1) {
            continue;
        }

        bool hostRemote = false;
        for (int distance = 5; distance <= 8 && !hostRemote; distance++) {
            for (int rotation = 0; rotation < ROTATION_COUNT && !hostRemote; rotation++) {
                int tile = tile_num_in_direction(exitGrid->tile, rotation, distance);
                hostRemote = hexGridTileIsValid(tile)
                    && obj_blocking_at(host, tile, exitGrid->elevation) == nullptr
                    && obj_move_to_tile(host, tile, exitGrid->elevation, nullptr) == 0;
            }
        }
        GameCommand readinessProbe;
        readinessProbe.sequence = CommandSequence { 1 };
        readinessProbe.playerId = kGuestPlayerId;
        readinessProbe.actorId = session.playerActorId(kGuestPlayerId);
        readinessProbe.expectedPhase = SessionPhase::Exploration;
        readinessProbe.expectedPhaseRevision = session.phaseRevision();
        readinessProbe.payload = ExitGridCommand { entry.first };
        AuthoritativeCommandResult readinessOutcome = hostRemote
            ? networkWorldProcessCommand(readinessProbe)
            : AuthoritativeCommandResult {};
        bool readinessRejected = readinessOutcome.result.status == CommandStatus::Rejected
            && readinessOutcome.result.rejection == CommandRejection::InvalidAction
            && !readinessOutcome.event.has_value()
            && map_data.field_34 != destinationMap;
        commandProcessor.reset();

        bool hostPlaced = false;
        for (int rotation = 0; rotation < ROTATION_COUNT && !hostPlaced; rotation++) {
            int tile = tile_num_in_direction(exitGrid->tile, rotation, 1);
            hostPlaced = hexGridTileIsValid(tile)
                && obj_blocking_at(host, tile, exitGrid->elevation) == nullptr
                && obj_move_to_tile(host, tile, exitGrid->elevation, nullptr) == 0;
        }
        int existingGuestCaps = item_caps_total(guest);
        if (!readinessRejected
            || !hostPlaced
            || (existingGuestCaps > 0 && item_caps_adjust(guest, -existingGuestCaps) != 0)
            || item_caps_adjust(guest, 11) != 0) {
            return std::nullopt;
        }
        Inventory& inventory = guest->data.inventory;
        bool capsRegistered = false;
        for (int index = 0; index < inventory.length; index++) {
            Object* item = inventory.items[index].item;
            if (item != nullptr && item->pid == PROTO_ID_MONEY && inventory.items[index].quantity == 11) {
                capsRegistered = static_cast<bool>(registerItem(item));
                break;
            }
        }
        if (!capsRegistered) {
            return std::nullopt;
        }
        return ExitGridSmokeFixture {
            entry.first,
            destinationMap,
            destinationElevation,
            11,
            session.playerActorId(kHostPlayerId),
            session.playerActorId(kGuestPlayerId),
        };
    }
    return std::nullopt;
}

bool networkWorldVerifyExitGridSmokeTest(const ExitGridSmokeFixture& fixture)
{
    Object* host = session.entities().findObject(fixture.hostActorId);
    Object* guest = session.entities().findObject(fixture.guestActorId);
    bool capsRegistered = false;
    if (guest != nullptr) {
        Inventory& inventory = guest->data.inventory;
        for (int index = 0; index < inventory.length; index++) {
            Object* item = inventory.items[index].item;
            std::optional<EntityId> itemId = item != nullptr
                ? session.entities().findEntity(item)
                : std::nullopt;
            if (item != nullptr
                && item->pid == PROTO_ID_MONEY
                && inventory.items[index].quantity == fixture.guestCaps
                && itemId.has_value()
                && std::any_of(worldItems.begin(), worldItems.end(), [&](const auto& entry) {
                    return entry.first == *itemId && entry.second == item;
                })) {
                capsRegistered = true;
                break;
            }
        }
    }
    return session.isActive()
        && session.playerActorId(kHostPlayerId) == fixture.hostActorId
        && session.playerActorId(kGuestPlayerId) == fixture.guestActorId
        && host != nullptr
        && guest != nullptr
        && host != guest
        && map_data.field_34 == fixture.destinationMap
        && session.phase() == SessionPhase::Exploration
        && host->elevation == fixture.destinationElevation
        && guest->elevation == fixture.destinationElevation
        && host->tile != guest->tile
        && capsRegistered
        && item_caps_total(guest) == fixture.guestCaps
        && map_elevation == fixture.destinationElevation;
}

std::optional<SceneryTransitionSmokeFixture> networkWorldPrepareSceneryTransitionSmokeTest(bool typedStairs, int targetTile, bool hostReady)
{
    if (!session.isActive()) {
        return std::nullopt;
    }
    Object* host = session.entities().findObject(session.playerActorId(kHostPlayerId));
    Object* guest = session.entities().findObject(session.playerActorId(kGuestPlayerId));
    if (host == nullptr || guest == nullptr) {
        return std::nullopt;
    }

    anim_stop();
    for (const auto& entry : worldScenery) {
        Object* transition = entry.second;
        int sceneryType = -1;
        if (!sceneryTransitionType(transition, sceneryType)
            || (typedStairs
                ? sceneryType != SCENERY_TYPE_STAIRS
                : sceneryType != SCENERY_TYPE_LADDER_UP
                    && sceneryType != SCENERY_TYPE_LADDER_DOWN)
            || transition->sid == -1
            || (targetTile >= 0 && (transition->tile != targetTile || transition->elevation != 0))) {
            continue;
        }
        bool guestPlaced = false;
        for (int rotation = 0; rotation < ROTATION_COUNT && !guestPlaced; rotation++) {
            int tile = tile_num_in_direction(transition->tile, rotation, 1);
            guestPlaced = hexGridTileIsValid(tile)
                && obj_blocking_at(guest, tile, transition->elevation) == nullptr
                && obj_move_to_tile(guest, tile, transition->elevation, nullptr) == 0;
        }
        auto placeHost = [&](int firstDistance, int lastDistance) {
            for (int distance = firstDistance; distance <= lastDistance; distance++) {
                for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
                    int tile = tile_num_in_direction(transition->tile, rotation, distance);
                    if (hexGridTileIsValid(tile)
                        && obj_blocking_at(host, tile, transition->elevation) == nullptr
                        && obj_move_to_tile(host, tile, transition->elevation, nullptr) == 0) {
                        return true;
                    }
                }
            }
            return false;
        };
        bool hostPlaced = placeHost(5, 8);
        if (!guestPlaced
            || !hostPlaced
            || obj_set_rotation(host, ROTATION_SE, nullptr) == -1
            || obj_set_rotation(guest, ROTATION_SE, nullptr) == -1) {
            continue;
        }
        if (typedStairs && hostReady && worldMode == NetworkLaunchMode::Host) {
            WorldSnapshot beforeRejection;
            if (!networkWorldCaptureSnapshot(EventSequence {}, beforeRejection)) {
                return std::nullopt;
            }
            GameCommand readinessProbe;
            readinessProbe.sequence = CommandSequence { 1 };
            readinessProbe.playerId = kGuestPlayerId;
            readinessProbe.actorId = session.playerActorId(kGuestPlayerId);
            readinessProbe.expectedPhase = SessionPhase::Exploration;
            readinessProbe.expectedPhaseRevision = session.phaseRevision();
            readinessProbe.payload = SceneryTransitionCommand { entry.first };
            AuthoritativeCommandResult rejection = networkWorldProcessCommand(readinessProbe);
            WorldSnapshot afterRejection;
            bool afterCaptured = networkWorldCaptureSnapshot(EventSequence {}, afterRejection);
            SnapshotDigestResult beforeDigest = computeSnapshotDigest(beforeRejection);
            SnapshotDigestResult afterDigest = afterCaptured
                ? computeSnapshotDigest(afterRejection)
                : SnapshotDigestResult {};
            bool worldUnchanged = beforeDigest && afterDigest
                && beforeDigest.digest.overall == afterDigest.digest.overall;
            if (!worldUnchanged) {
                std::fprintf(stderr, "Denied typed stair changed section=%d captured=%d.\n",
                    afterCaptured
                        ? static_cast<int>(firstDivergentSection(beforeDigest.digest, afterDigest.digest))
                        : -1,
                    afterCaptured ? 1 : 0);
            }
            bool rejected = rejection.result.status == CommandStatus::Rejected
                && rejection.result.rejection == CommandRejection::InvalidAction
                && !rejection.event.has_value()
                && map_data.field_34 == MAP_CHILDRN2
                && worldUnchanged;
            commandProcessor.reset();
            if (!rejected) {
                return std::nullopt;
            }
        }
        if (hostReady && !placeHost(2, 4)) {
            return std::nullopt;
        }
        int destinationMap = typedStairs && hostReady ? MAP_CHILDRN1 : map_data.field_34;
        int guestCaps = 0;
        if (typedStairs && hostReady) {
            int existingCaps = item_caps_total(guest);
            if ((existingCaps > 0 && item_caps_adjust(guest, -existingCaps) != 0)
                || item_caps_adjust(guest, 11) != 0) {
                return std::nullopt;
            }
            guestCaps = 11;
            bool registered = false;
            for (int index = 0; index < guest->data.inventory.length; index++) {
                Object* item = guest->data.inventory.items[index].item;
                if (item != nullptr && item->pid == PROTO_ID_MONEY
                    && guest->data.inventory.items[index].quantity == guestCaps) {
                    registered = static_cast<bool>(registerItem(item));
                    break;
                }
            }
            if (!registered) {
                return std::nullopt;
            }
        }
        return SceneryTransitionSmokeFixture {
            entry.first,
            destinationMap,
            map_data.field_34,
            typedStairs ? 1 : -1,
            guestCaps,
            host->tile,
            host->elevation,
            host->rotation,
            guest->tile,
            guest->elevation,
            session.playerActorId(kHostPlayerId),
            session.playerActorId(kGuestPlayerId),
        };
    }
    return std::nullopt;
}

bool networkWorldVerifySceneryTransitionSmokeTest(const SceneryTransitionSmokeFixture& fixture)
{
    Object* host = session.entities().findObject(fixture.hostActorId);
    Object* guest = session.entities().findObject(fixture.guestActorId);
    Object* localActor = localPlayerActor();
    bool crossMap = fixture.map != fixture.sourceMap;
    bool capsRegistered = !crossMap;
    if (crossMap && guest != nullptr) {
        for (int index = 0; index < guest->data.inventory.length; index++) {
            Object* item = guest->data.inventory.items[index].item;
            std::optional<EntityId> itemId = item != nullptr
                ? session.entities().findEntity(item)
                : std::nullopt;
            if (item != nullptr && item->pid == PROTO_ID_MONEY
                && guest->data.inventory.items[index].quantity == fixture.guestCaps
                && itemId.has_value()) {
                capsRegistered = true;
                break;
            }
        }
    }
    bool verified = session.isActive()
        && session.playerActorId(kHostPlayerId) == fixture.hostActorId
        && session.playerActorId(kGuestPlayerId) == fixture.guestActorId
        && host != nullptr
        && guest != nullptr
        && localActor != nullptr
        && map_data.field_34 == fixture.map
        && session.phase() == SessionPhase::Exploration
        && (crossMap || (host->tile == fixture.hostTile
                && host->elevation == fixture.hostElevation
                && host->rotation == fixture.hostRotation))
        && (!crossMap || (host->elevation == fixture.destinationElevation
                && guest->elevation == fixture.destinationElevation
                && capsRegistered
                && item_caps_total(guest) == fixture.guestCaps))
        && (guest->tile != fixture.guestStartingTile
            || guest->elevation != fixture.guestStartingElevation)
        && (host->tile != guest->tile || host->elevation != guest->elevation)
        && map_elevation == localActor->elevation;
    if (!verified) {
        std::fprintf(stderr,
            "Multiplayer scenery transition fixture failed: map=%d/%d host=%d,%d,%d/%d,%d,%d guest=%d,%d/%d,%d local_elevation=%d/%d phase=%d.\n",
            map_data.field_34,
            fixture.map,
            host != nullptr ? host->tile : -1,
            host != nullptr ? host->elevation : -1,
            host != nullptr ? host->rotation : -1,
            fixture.hostTile,
            fixture.hostElevation,
            fixture.hostRotation,
            guest != nullptr ? guest->tile : -1,
            guest != nullptr ? guest->elevation : -1,
            fixture.guestStartingTile,
            fixture.guestStartingElevation,
            map_elevation,
            localActor != nullptr ? localActor->elevation : -1,
            static_cast<int>(session.phase()));
    }
    return verified;
}

static bool nestedCapsSmokeTest()
{
    std::array<Object*, 3> wallets {};
    bool created = true;
    for (Object*& wallet : wallets) {
        if (obj_pid_new(&wallet, 211) != 0 || wallet == nullptr
            || item_get_type(wallet) != ITEM_TYPE_CONTAINER) { created = false; break; }
    }
    bool passed = created
        && item_add_force(wallets[0], wallets[1], 1) == 0
        && item_add_force(wallets[0], wallets[2], 1) == 0
        && item_caps_adjust(wallets[0], 2) == 0
        && item_caps_adjust(wallets[1], 3) == 0
        && item_caps_adjust(wallets[2], 4) == 0
        && item_caps_total(wallets[0]) == 9
        && item_caps_adjust(wallets[0], -7) == 0
        && item_caps_total(wallets[0]) == 2
        && item_caps_total(wallets[1]) == 0 && item_caps_total(wallets[2]) == 2;
    for (auto it = wallets.rbegin(); it != wallets.rend(); ++it) {
        Object* wallet = *it;
        if (wallet == nullptr) continue;
        item_caps_adjust(wallet, -item_caps_total(wallet));
        if (wallet->owner != nullptr) item_remove_mult(wallet->owner, wallet, 1);
        obj_erase_object(wallet, nullptr);
    }
    std::fprintf(stdout, "NATIVE_NESTED_CAPS_%s initial=9 spent=7 remaining=2\n", passed ? "PASS" : "FAIL");
    return passed;
}

static bool ownedContainerSmokeTest(Object* actor)
{
    ScopedLocalPlayerBinding binding(actor);
    Object* bag = nullptr;
    Object* inner = nullptr;
    Object* ammo = nullptr;
    bool passed = false;
    if (obj_pid_new(&bag, 211) == 0 && obj_pid_new(&inner, 211) == 0
        && obj_pid_new(&ammo, 10) == 0
        && item_add_force(actor, bag, 1) == 0
        && item_add_force(bag, inner, 1) == 0
        && item_add_force(bag, ammo, 3) == 0
        && registerUntrackedInventory(actor)) {
        auto actorId = session.entities().findEntity(actor).value_or(EntityId {});
        auto bagId = session.entities().findEntity(bag).value_or(EntityId {});
        auto innerId = session.entities().findEntity(inner).value_or(EntityId {});
        auto ammoId = session.entities().findEntity(ammo).value_or(EntityId {});
        InventoryTransferCommand put { bagId, innerId, ammoId, 2, 3, {} };
        auto deposit = commandExecutor.transferInventory(actor, bag, inner, ammo, put);
        InventoryTransferCommand nested { innerId, bagId, ammoId, 1, 2, {} };
        auto move = commandExecutor.transferInventory(actor, inner, bag, ammo, nested);
        InventoryTransferCommand take { bagId, innerId, ammoId, 1, 2, {} };
        auto withdraw = commandExecutor.transferInventory(actor, bag, inner, ammo, take);
        // Withdrawing merges equivalent ammunition; resolve the surviving item
        // from inventory rather than dereferencing the discarded representative.
        ammo = nullptr;
        InventoryTransferCommand cycle { actorId, innerId, bagId, 1, 1, {} };
        auto rejected = commandExecutor.transferInventory(actor, actor, inner, bag, cycle);
        Object* other = networkWorldPlayerActor(kHostPlayerId);
        InventoryTransferCommand steal { actorId, innerId, bagId, 1, 1, {} };
        auto foreign = commandExecutor.transferInventory(other, actor, inner, bag, steal);
        std::fprintf(stdout, "OWNED_CONTAINER_DIAGNOSTICS deposit=%d split=%u move=%d split2=%u withdraw=%d cycle=%d foreign=%d\n", static_cast<int>(deposit.status), deposit.remainderItemId.value,
            static_cast<int>(move.status), move.remainderItemId.value,
            static_cast<int>(withdraw.status), static_cast<int>(rejected.status), static_cast<int>(foreign.status));
        passed = deposit.status == CommandExecutionStatus::Applied
            && isValid(deposit.remainderItemId)
            && move.status == CommandExecutionStatus::Applied
            && isValid(move.remainderItemId)
            && withdraw.status == CommandExecutionStatus::Applied
            && rejected.status == CommandExecutionStatus::InvalidAction
            && foreign.status == CommandExecutionStatus::InvalidAction
            && bag->owner == actor && inner->owner == bag;
    }
    // These fixture objects have no gameplay value. Remove contents before
    // deleting their holders so the normal native cleanup owns every object.
    if (ammo != nullptr && ammo->owner == nullptr) obj_erase_object(ammo, nullptr);
    if (inner != nullptr && inner->owner == nullptr) obj_erase_object(inner, nullptr);
    if (bag != nullptr) {
        if (inner != nullptr && inner->owner == bag) {
            while (inner->data.inventory.length > 0) {
                auto entry = inner->data.inventory.items[0];
                item_remove_mult(inner, entry.item, entry.quantity);
                obj_erase_object(entry.item, nullptr);
            }
            item_remove_mult(bag, inner, 1);
            obj_erase_object(inner, nullptr);
        }
        while (bag->data.inventory.length > 0) {
            auto entry = bag->data.inventory.items[0];
            item_remove_mult(bag, entry.item, entry.quantity);
            obj_erase_object(entry.item, nullptr);
        }
        if (bag->owner != nullptr) item_remove_mult(bag->owner, bag, 1);
        obj_erase_object(bag, nullptr);
    }
    std::fprintf(stdout, "NATIVE_OWNED_CONTAINER_%s split=1 nested=1 withdraw=1 cycle_rejected=1 foreign_rejected=1\n", passed ? "PASS" : "FAIL");
    return passed;
}

static bool lootDirectionSmokeTest(Object* actor, Object* target)
{
    if (worldMode != NetworkLaunchMode::Host) return true;
    auto* player = playerStateForActor(actor);
    Object* host = networkWorldPlayerActor(kHostPlayerId);
    auto actorId = session.entities().findEntity(actor);
    auto targetId = session.entities().findEntity(target);
    if (player == nullptr || host == nullptr || !actorId || !targetId) return false;
    ScopedActingPlayerContext context(*player, actor);
    Proto* targetPrototype = nullptr;
    if (proto_ptr(target->pid, &targetPrototype) == -1) return false;
    // Native inventory refuses deposits into animals. Exercise a human corpse
    // in this cave fixture, then restore the prototype before gameplay resumes.
    int bodyType = targetPrototype->critter.data.bodyType;
    targetPrototype->critter.data.bodyType = BODY_TYPE_BIPED;
    LootDistributionState distribution = lootDistribution.state();
    auto previousLoot = activeLootTargets.find(actor);
    Object* previousTarget = previousLoot != activeLootTargets.end() ? previousLoot->second : nullptr;
    int results = target->data.critter.combat.results;
    target->data.critter.combat.results &= ~(DAM_DEAD | DAM_KNOCKED_OUT | DAM_LOSE_TURN);
    bool activeRejected = commandExecutor.loot(actor, target) == CommandExecutionStatus::InvalidAction;
    target->data.critter.combat.results = results;
    activeLootTargets[actor] = target;

    int hostCaps = item_caps_total(host);
    int actorCaps = item_caps_total(actor);
    int targetCaps = item_caps_total(target);
    bool capsDeposited = false;
    bool capsTaken = false;
    if (item_caps_adjust(actor, 7) == 0 && registerUntrackedInventory(actor)) {
        Object* caps = nullptr;
        for (int index = 0; index < actor->data.inventory.length; ++index) {
            Object* candidate = actor->data.inventory.items[index].item;
            if (candidate->pid == PROTO_ID_MONEY) { caps = candidate; break; }
        }
        if (caps != nullptr) {
            InventoryTransferCommand command { *actorId, *targetId,
                session.entities().findEntity(caps).value_or(EntityId {}), 3,
                static_cast<std::uint32_t>(item_count(actor, caps)), {} };
            auto deposit = commandExecutor.transferInventory(actor, actor, target, caps, command);
            capsDeposited = deposit.status == CommandExecutionStatus::Applied
                && deposit.capShares.empty() && item_caps_total(actor) == actorCaps + 4
                && item_caps_total(target) == targetCaps + 3 && item_caps_total(host) == hostCaps
                && lootDistribution.state().nextCapExtraIndex == distribution.nextCapExtraIndex;
            if (capsDeposited && registerUntrackedInventory(target)) {
                Object* deposited = nullptr;
                for (int index = 0; index < target->data.inventory.length; ++index) {
                    Object* candidate = target->data.inventory.items[index].item;
                    if (candidate->pid == PROTO_ID_MONEY) { deposited = candidate; break; }
                }
                command.sourceId = *targetId;
                command.destinationId = *actorId;
                command.itemId = session.entities().findEntity(deposited).value_or(EntityId {});
                command.sourceQuantity = static_cast<std::uint32_t>(item_count(target, deposited));
                auto take = commandExecutor.transferInventory(actor, target, actor, deposited, command);
                capsTaken = take.status == CommandExecutionStatus::Applied
                    && take.capShares.size() == 2 && item_caps_total(target) == targetCaps
                    && item_caps_total(host) + item_caps_total(actor) == hostCaps + actorCaps + 7;
            }
        }
    }
    item_caps_adjust(host, hostCaps - item_caps_total(host));
    item_caps_adjust(actor, actorCaps - item_caps_total(actor));
    item_caps_adjust(target, targetCaps - item_caps_total(target));

    Object* weapon = nullptr;
    bool equippedRejected = false;
    bool equippedDropRejected = false;
    bool itemDeposited = false;
    bool itemTaken = false;
    if (obj_pid_new(&weapon, 1) == 0 && weapon != nullptr
        && item_add_force(actor, weapon, 1) == 0) {
        auto registration = registerItem(weapon);
        if (registration) {
            InventoryTransferCommand command { *actorId, *targetId, registration.entityId, 1, 1, {} };
            int flags = weapon->flags;
            weapon->flags |= OBJECT_IN_RIGHT_HAND;
            auto equipped = commandExecutor.transferInventory(actor, actor, target, weapon, command);
            equippedRejected = equipped.status == CommandExecutionStatus::InvalidAction && weapon->owner == actor;
            ItemDropCommand drop { *actorId, registration.entityId, 1, 1, {} };
            auto equippedDrop = commandExecutor.dropItem(actor, actor, weapon, drop);
            equippedDropRejected = equippedDrop.status == CommandExecutionStatus::InvalidAction && weapon->owner == actor;
            weapon->flags = flags;
            auto deposit = commandExecutor.transferInventory(actor, actor, target, weapon, command);
            itemDeposited = deposit.status == CommandExecutionStatus::Applied && weapon->owner == target
                && !isValid(deposit.destinationId)
                && lootDistribution.state().nextLootPriorityIndex == distribution.nextLootPriorityIndex;
            if (itemDeposited) {
                command.sourceId = *targetId;
                command.destinationId = *actorId;
                auto take = commandExecutor.transferInventory(actor, target, actor, weapon, command);
                itemTaken = take.status == CommandExecutionStatus::Applied
                    && isPlayerActor(weapon->owner) && isValid(take.destinationId);
            }
        }
    }
    if (weapon != nullptr) {
        if (weapon->owner != nullptr) item_remove_mult(weapon->owner, weapon, 1);
        obj_erase_object(weapon, nullptr);
    }
    lootDistribution.restore(distribution);
    targetPrototype->critter.data.bodyType = bodyType;
    if (previousTarget != nullptr) activeLootTargets[actor] = previousTarget;
    else activeLootTargets.erase(actor);
    bool passed = activeRejected && equippedRejected && equippedDropRejected && capsDeposited && capsTaken && itemDeposited && itemTaken
        && nestedCapsSmokeTest() && ownedContainerSmokeTest(actor);
    std::fprintf(stdout, "NATIVE_LOOT_DIRECTION_%s active_rejected=%d equipped_rejected=%d equipped_drop_rejected=%d caps_deposit=%d caps_split=%d item_deposit=%d item_priority=%d\n",
        passed ? "PASS" : "FAIL", activeRejected, equippedRejected, equippedDropRejected, capsDeposited, capsTaken, itemDeposited, itemTaken);
    return passed;
}

bool networkWorldVerifyLootRangeSmokeTest(EntityId targetId)
{
    Object* actor = session.entities().findObject(session.playerActorId(kGuestPlayerId));
    Object* target = session.entities().findObject(targetId);
    if (!session.isActive() || !lootTargetIsInRange(actor, target)) {
        return false;
    }

    int adjacentTile = actor->tile;
    int elevation = actor->elevation;
    int remoteTile = -1;
    for (int distance = 2; distance <= 4 && remoteTile == -1; distance++) {
        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            int candidate = tile_num_in_direction(target->tile, rotation, distance);
            std::array<unsigned char, kMaximumMovementPathLength> path;
            if (hexGridTileIsValid(candidate)
                && make_path(actor, adjacentTile, candidate, path.data(), 1) > 0
                && obj_blocking_at(actor, candidate, elevation) == nullptr) {
                remoteTile = candidate;
                break;
            }
        }
    }
    if (remoteTile == -1 || obj_move_to_tile(actor, remoteTile, elevation, nullptr) == -1) {
        return false;
    }

    GameCommand command;
    command.sequence = CommandSequence { 1 };
    command.playerId = kGuestPlayerId;
    command.actorId = session.playerActorId(kGuestPlayerId);
    command.expectedPhase = SessionPhase::Exploration;
    command.expectedPhaseRevision = session.phaseRevision();
    command.payload = LootCommand { targetId };
    AuthoritativeCommandResult outcome = networkWorldProcessCommand(command);
    bool approached = worldMode != NetworkLaunchMode::Host || (outcome.result.status == CommandStatus::Accepted
        && outcome.event.has_value() && lootTargetIsInRange(actor, target)
        && activeLootTargets.count(actor) != 0 && activeLootTargets.at(actor) == target);
    activeLootTargets.erase(actor);
    command.sequence = CommandSequence { 2 };
    bool movedFloor = obj_move_to_tile(actor, adjacentTile, (elevation + 1) % ELEVATION_COUNT, nullptr) == 0;
    outcome = networkWorldProcessCommand(command);
    bool rejected = movedFloor && outcome.result.status == CommandStatus::Rejected
        && outcome.result.rejection == CommandRejection::InvalidAction
        && !outcome.event.has_value() && activeLootTargets.count(actor) == 0;

    commandProcessor.reset();
    bool restored = obj_move_to_tile(actor, adjacentTile, elevation, nullptr) == 0
        && lootTargetIsInRange(actor, target);
    return approached && rejected && restored && lootDirectionSmokeTest(actor, target);
}

std::optional<EntityId> networkWorldPreparePlayerTransferSmokeTest()
{
    if (!session.isActive()) {
        return std::nullopt;
    }
    Object* hostActor = session.entities().findObject(session.playerActorId(kHostPlayerId));
    Object* guestActor = session.entities().findObject(session.playerActorId(kGuestPlayerId));
    if (hostActor == nullptr || guestActor == nullptr) {
        return std::nullopt;
    }

    anim_stop();
    bool placed = false;
    for (int rotation = 0; rotation < ROTATION_COUNT && !placed; rotation++) {
        int tile = tile_num_in_direction(hostActor->tile, rotation, 1);
        placed = hexGridTileIsValid(tile)
            && obj_blocking_at(guestActor, tile, hostActor->elevation) == nullptr
            && obj_move_to_tile(guestActor, tile, hostActor->elevation, nullptr) == 0;
    }
    int existingHostCaps = item_caps_total(hostActor);
    int existingGuestCaps = item_caps_total(guestActor);
    if (!placed
        || (existingHostCaps > 0 && item_caps_adjust(hostActor, -existingHostCaps) != 0)
        || (existingGuestCaps > 0 && item_caps_adjust(guestActor, -existingGuestCaps) != 0)
        || item_caps_adjust(guestActor, 7) != 0) {
        std::fprintf(stderr, "PLAYER_TRANSFER_PREPARE_FAIL placed=%d host_caps=%d guest_caps=%d\n",
            placed, item_caps_total(hostActor), item_caps_total(guestActor));
        return std::nullopt;
    }

    Inventory& inventory = guestActor->data.inventory;
    for (int index = 0; index < inventory.length; index++) {
        Object* item = inventory.items[index].item;
        if (item != nullptr && item->pid == PROTO_ID_MONEY && inventory.items[index].quantity == 7) {
            EntityRegistrationResult registration = registerItem(item);
            return registration ? std::optional<EntityId>(registration.entityId) : std::nullopt;
        }
    }
    std::fprintf(stderr, "PLAYER_TRANSFER_CAPS_MISSING total=%d entries=%d\n", item_caps_total(guestActor), inventory.length);
    return std::nullopt;
}

bool networkWorldPrepareRecoverySmokeTest()
{
    if (!networkWorldPreparePlayerTransferSmokeTest().has_value()) {
        return false;
    }
    if (worldMode == NetworkLaunchMode::Host) {
        Object* hostActor = networkWorldPlayerActor(kHostPlayerId);
        if (hostActor == nullptr) return false;
        playerStateForActor(hostActor)->build.activeHand = 0;
        intface_select_item(0);
        hostActor->data.critter.hp = 17;
        hostActor->data.critter.poison = 7;
        hostActor->data.critter.radiation = 11;
        hostActor->data.critter.combat.results |= DAM_CRIP_ARM_LEFT;
        Object* guestActor = networkWorldPlayerActor(kGuestPlayerId);
        auto* guestPlayer = playerStateForActor(guestActor);
        if (guestPlayer == nullptr) return false;
        ScopedActingPlayerContext guestContext(*guestPlayer, guestActor);
        guestPlayer->build.activeHand = 1;
        pc_flag_on(PC_FLAG_SNEAKING);
        guestPlayer->build.healingSkillUses[0] = { 100, 200, 300 };
        item_d_set_addict(PROTO_ID_BUFF_OUT);
        auto* drug = static_cast<DrugEffectEvent*>(mem_malloc(sizeof(DrugEffectEvent)));
        auto* withdrawal = static_cast<WithdrawalEvent*>(mem_malloc(sizeof(WithdrawalEvent)));
        if (drug == nullptr || withdrawal == nullptr) { mem_free(drug); mem_free(withdrawal); return false; }
        *drug = DrugEffectEvent { PROTO_ID_BUFF_OUT, { STAT_AGILITY, -1, -1 }, { 2, 0, 0 } };
        *withdrawal = WithdrawalEvent { 1, PROTO_ID_BUFF_OUT, PERK_BUFFOUT_ADDICTION };
        if (queue_add(1000000, guestActor, drug, EVENT_TYPE_DRUG) != 0) {
            mem_free(drug); mem_free(withdrawal); return false;
        }
        if (queue_add(1000000, guestActor, withdrawal, EVENT_TYPE_WITHDRAWAL) != 0) {
            mem_free(withdrawal); return false;
        }
        // Keep one player incapacitated across the real disk/session boundary.
        guestActor->data.critter.combat.results |= DAM_KNOCKED_OUT;
        queue_remove_this(guestActor, EVENT_TYPE_KNOCKOUT);
        if (queue_add(500000, guestActor, nullptr, EVENT_TYPE_KNOCKOUT) != 0) return false;
        publishSharedActivity(SharedActivityKind::WorldOutcome, 77, 1,
            "Recovery smoke checkpoint");
    } else if (worldMode == NetworkLaunchMode::Join) {
        // Reproduce a fresh MAP missing scenery present in the saved host map.
        // The next checkpoint is the durable Ending boundary.
        if (worldScenery.empty()) return false;
        auto missing = worldScenery.back();
        worldScenery.pop_back();
        session.entities().unregisterEntity(missing.first);
        if (obj_erase_object(missing.second, nullptr) == -1) return false;
        std::fprintf(stderr, "NATIVE_ENDING_SCENERY_DIVERGENCE missing=1\n");
    }
    return true;
}

bool networkWorldVerifyRecoverySmokeTest()
{
    Object* host = networkWorldPlayerActor(kHostPlayerId);
    Object* guest = networkWorldPlayerActor(kGuestPlayerId);
    auto* player = playerStateForActor(guest);
    std::vector<QueueEventState> timers;
    bool knockoutTimerIntact = queue_capture_state(timers)
        && std::count_if(timers.begin(), timers.end(), [&](const auto& timer) {
            return timer.owner == guest && timer.eventType == EVENT_TYPE_KNOCKOUT
                && timer.time == game_time() + 500000;
        }) == 1;
    return session.isActive()
        && session.phase() == SessionPhase::Exploration
        && host != nullptr && host->data.critter.hp == 17
        && host->data.critter.poison == 7 && host->data.critter.radiation == 11
        && (host->data.critter.combat.results & DAM_CRIP_ARM_LEFT) != 0
        && guest != nullptr && player != nullptr
        && (guest->data.critter.combat.results & DAM_KNOCKED_OUT) != 0
        && !networkWorldPartyDefeated() && knockoutTimerIntact
        && playerStateForActor(host)->build.activeHand == 0 && player->build.activeHand == 1
        && intface_is_item_right_hand() == localPlayerState()->build.activeHand
        && (player->build.prototypeFlags & (1 << PC_FLAG_SNEAKING)) != 0
        && player->build.healingSkillUses[0] == std::array<std::int32_t, 3> { 100, 200, 300 }
        && player->build.addictions != 0
        && queue_find(guest, EVENT_TYPE_SNEAK)
        && queue_find(guest, EVENT_TYPE_DRUG)
        && queue_find(guest, EVENT_TYPE_WITHDRAWAL)
        && item_caps_total(guest) == 7
        && !sharedActivity.empty()
        && sharedActivity.back().kind == SharedActivityKind::WorldOutcome
        && sharedActivity.back().subject == 77
        && sharedActivity.back().text == "Recovery smoke checkpoint";
}

bool networkWorldVerifyPlayerTransferRangeSmokeTest(EntityId itemId)
{
    Object* hostActor = session.entities().findObject(session.playerActorId(kHostPlayerId));
    Object* guestActor = session.entities().findObject(session.playerActorId(kGuestPlayerId));
    Object* item = session.entities().findObject(itemId);
    ItemDescriptor descriptor;
    if (!session.isActive()
        || hostActor == nullptr
        || guestActor == nullptr
        || item == nullptr
        || item->owner != guestActor
        || item_count(guestActor, item) != 7
        || !isAdjacentPlayerActor(guestActor, hostActor)
        || !describeItem(item, descriptor)) {
        std::fprintf(stderr, "PLAYER_TRANSFER_FIXTURE_INVALID active=%d host=%p guest=%p item=%p owner=%p quantity=%d adjacent=%d\n",
            session.isActive(), static_cast<void*>(hostActor), static_cast<void*>(guestActor), static_cast<void*>(item),
            item != nullptr ? static_cast<void*>(item->owner) : nullptr,
            guestActor != nullptr && item != nullptr ? item_count(guestActor, item) : -1,
            isAdjacentPlayerActor(guestActor, hostActor));
        return false;
    }

    int adjacentTile = guestActor->tile;
    int elevation = guestActor->elevation;
    int remoteTile = -1;
    for (int distance = 2; distance <= 4 && remoteTile == -1; distance++) {
        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            int candidate = tile_num_in_direction(hostActor->tile, rotation, distance);
            if (hexGridTileIsValid(candidate)
                && obj_blocking_at(guestActor, candidate, elevation) == nullptr) {
                remoteTile = candidate;
                break;
            }
        }
    }
    if (remoteTile == -1 || obj_move_to_tile(guestActor, remoteTile, elevation, nullptr) == -1) {
        return false;
    }

    GameCommand command;
    command.sequence = CommandSequence { 1 };
    command.playerId = kGuestPlayerId;
    command.actorId = session.playerActorId(kGuestPlayerId);
    command.expectedPhase = SessionPhase::Exploration;
    command.expectedPhaseRevision = session.phaseRevision();
    command.payload = InventoryTransferCommand {
        session.playerActorId(kGuestPlayerId),
        session.playerActorId(kHostPlayerId),
        itemId,
        3,
        7,
        descriptor,
    };
    AuthoritativeCommandResult outcome = networkWorldProcessCommand(command);
    bool rejected = outcome.result.status == CommandStatus::Rejected
        && outcome.result.rejection == CommandRejection::InvalidAction
        && !outcome.event.has_value()
        && item->owner == guestActor
        && item_count(guestActor, item) == 7
        && item_caps_total(hostActor) == 0;

    bool restored = obj_move_to_tile(guestActor, adjacentTile, elevation, nullptr) == 0
        && isAdjacentPlayerActor(guestActor, hostActor);
    GameCommand playerLoot = command;
    playerLoot.sequence = CommandSequence { 2 };
    playerLoot.payload = LootCommand { session.playerActorId(kHostPlayerId) };
    AuthoritativeCommandResult playerLootOutcome = networkWorldProcessCommand(playerLoot);
    bool playerLootRejected = playerLootOutcome.result.status == CommandStatus::Rejected
        && playerLootOutcome.result.rejection == CommandRejection::InvalidAction
        && !playerLootOutcome.event.has_value()
        && activeLootTargets.find(guestActor) == activeLootTargets.end();

    command.sequence = CommandSequence { 3 };
    command.payload = InventoryTransferCommand {
        session.playerActorId(kGuestPlayerId),
        session.playerActorId(kHostPlayerId),
        itemId,
        7,
        7,
        descriptor,
    };
    AuthoritativeCommandResult giftOutcome = networkWorldProcessCommand(command);
    const auto* giftEvent = giftOutcome.event.has_value()
        ? std::get_if<InventoryTransferredEvent>(&giftOutcome.event->payload)
        : nullptr;
    bool gifted = giftOutcome.result.status == CommandStatus::Accepted
        && giftEvent != nullptr
        && !isValid(giftEvent->remainderItemId)
        && item->owner == hostActor
        && item_count(hostActor, item) == 7;

    command.sequence = CommandSequence { 4 };
    command.payload = InventoryTransferCommand {
        session.playerActorId(kHostPlayerId),
        session.playerActorId(kGuestPlayerId),
        itemId,
        3,
        7,
        descriptor,
    };
    AuthoritativeCommandResult takeOutcome = networkWorldProcessCommand(command);
    bool takingRejected = takeOutcome.result.status == CommandStatus::Rejected
        && takeOutcome.result.rejection == CommandRejection::InvalidAction
        && !takeOutcome.event.has_value()
        && item->owner == hostActor
        && item_count(hostActor, item) == 7;
    bool rolledBack = giftEvent != nullptr
        && networkWorldApplyInventoryTransfer(*giftEvent, true)
        && item->owner == guestActor
        && item_count(guestActor, item) == 7
        && item_caps_total(hostActor) == 0;

    commandProcessor.reset();
    if (!(rejected && restored && playerLootRejected && gifted && takingRejected && rolledBack)) {
        std::fprintf(stderr, "PLAYER_TRANSFER_RANGE_FAIL far=%d restored=%d player_loot=%d gift=%d taking=%d rollback=%d gift_status=%d gift_rejection=%d\n",
            rejected, restored, playerLootRejected, gifted, takingRejected, rolledBack,
            static_cast<int>(giftOutcome.result.status), static_cast<int>(giftOutcome.result.rejection));
    }
    return rejected && restored && playerLootRejected && gifted && takingRejected && rolledBack;
}

static bool worldMapControllerRulesSmokeTest()
{
    if (worldMode != NetworkLaunchMode::Host) return true;
    auto* guest = session.players().find(kGuestPlayerId);
    auto* host = session.players().find(kHostPlayerId);
    Object* guestActor = session.entities().findObject(session.playerActorId(kGuestPlayerId));
    Object* hostActor = session.entities().findObject(session.playerActorId(kHostPlayerId));
    if (guest == nullptr || host == nullptr || guestActor == nullptr || hostActor == nullptr
        || networkWorldWorldMapControllerActor() != guestActor) return false;
    CharacterBuild savedGuest = guest->build;
    WorldMapTravelProgress savedProgress;
    worldmap_capture_travel_progress(savedProgress);
    WorldMapState position;
    worldmap_capture_state(position);
    guest->build.skillPoints[SKILL_OUTDOORSMAN] = 200;
    guest->build.perkRanks[PERK_PATHFINDER] = 2;
    bool passed;
    {
        ScopedActingPlayerContext hostContext(*host, hostActor);
        passed = worldmap_authoritative_travel_begin(position.x, position.y);
        WorldMapTravelProgress progress;
        worldmap_capture_travel_progress(progress);
        passed = passed && progress.dayLength == 120 && progress.timeAdder == 3600;
    }
    guest->build = savedGuest;
    passed = worldmap_apply_travel_progress(savedProgress) && passed;
    std::fprintf(stderr, "NATIVE_WORLD_MAP_CONTROLLER_RULES_%s guest_controller=1 outdoorsman=100 pathfinder=2 day=120 ticks=3600\n",
        passed ? "PASS" : "FAIL");
    return passed;
}

bool networkWorldRunSharedModalSmokeTest()
{
    if (!session.isActive() || session.phase() != SessionPhase::Exploration) {
        return false;
    }
    EntityId actorId = session.playerActorId(kGuestPlayerId);
    // Some smoke maps have no world-map exit. Exercise the installed exit
    // whenever one is present without making unrelated map fixtures fail.
    bool worldMapExitProposes = true;
    Object* guestForExit = session.entities().findObject(actorId);
    Object* hostForExit = session.entities().findObject(session.playerActorId(kHostPlayerId));
    if (guestForExit != nullptr && hostForExit != nullptr) {
        for (const auto& entry : worldExitGrids) {
            Object* exitGrid = entry.second;
            if (!isExitGrid(exitGrid) || !isWorldMapDestination(exitGrid->data.misc.map)) continue;
            int oldGuestTile = guestForExit->tile;
            int oldGuestElevation = guestForExit->elevation;
            int oldHostTile = hostForExit->tile;
            int oldHostElevation = hostForExit->elevation;
            bool positioned = obj_move_to_tile(guestForExit, exitGrid->tile, exitGrid->elevation, nullptr) == 0;
            bool hostPositioned = false;
            for (int distance = 1; distance <= 4 && !hostPositioned; distance++) {
                for (int rotation = 0; rotation < ROTATION_COUNT && !hostPositioned; rotation++) {
                    int tile = tile_num_in_direction(exitGrid->tile, rotation, distance);
                    hostPositioned = hexGridTileIsValid(tile)
                        && obj_blocking_at(hostForExit, tile, exitGrid->elevation) == nullptr
                        && obj_move_to_tile(hostForExit, tile, exitGrid->elevation, nullptr) == 0;
                }
            }
            GameCommand entryCommand;
            entryCommand.sequence = CommandSequence { 1 };
            entryCommand.playerId = kGuestPlayerId;
            entryCommand.actorId = actorId;
            entryCommand.expectedPhase = SessionPhase::Exploration;
            entryCommand.expectedPhaseRevision = session.phaseRevision();
            entryCommand.payload = ExitGridCommand { entry.first };
            AuthoritativeCommandResult proposed = positioned && hostPositioned
                ? networkWorldProcessCommand(entryCommand)
                : AuthoritativeCommandResult {};
            const auto* proposalEvent = proposed.event.has_value()
                ? std::get_if<SharedModalStateChangedEvent>(&proposed.event->payload)
                : nullptr;
            worldMapExitProposes = proposed.result.status == CommandStatus::Accepted
                && proposalEvent != nullptr
                && proposalEvent->actorId == actorId
                && proposalEvent->kind == SharedModalKind::WorldMap
                && proposalEvent->open
                && proposalEvent->phase == SessionPhase::Exploration
                && session.phase() == SessionPhase::Exploration;
            std::fprintf(stderr, "NATIVE_WORLD_EXIT_PROPOSAL map=%d tile=%d accepted=%d\n",
                exitGrid->data.misc.map, exitGrid->tile, worldMapExitProposes ? 1 : 0);
            GameCommand withdraw = entryCommand;
            withdraw.sequence = CommandSequence { 2 };
            withdraw.payload = SharedModalCommand { SharedModalKind::WorldMap, false };
            worldMapExitProposes = worldMapExitProposes
                && networkWorldProcessCommand(withdraw).result.status == CommandStatus::Accepted
                && !pendingWorldMapProposal.has_value();
            worldMapExitProposes = obj_move_to_tile(guestForExit, oldGuestTile, oldGuestElevation, nullptr) == 0
                && obj_move_to_tile(hostForExit, oldHostTile, oldHostElevation, nullptr) == 0
                && worldMapExitProposes;
            commandProcessor.reset();
            break;
        }
    }
    int startingWorldTime = game_time();
    struct RestoreWorldMapFixture {
        WorldMapState saved;
        ~RestoreWorldMapFixture() { worldmap_apply_state(saved); }
    } restoreWorldMap;
    worldmap_capture_state(restoreWorldMap.saved);
    // These controller checks cancel without leaving the installed map.
    // Use a terrain position so cancellation does not enter a city's main
    // map and invalidate the NPC/item pointers used by the next fixture.
    WorldMapState cancellationPosition = restoreWorldMap.saved;
    cancellationPosition.x = 725;
    cancellationPosition.y = 616;
    if (!worldmap_apply_state(cancellationPosition)) return false;
    WorldMapState startingWorldMap;
    worldmap_capture_state(startingWorldMap);
    // The headless stepper must never execute on a replica. An already-reached
    // target is also a useful no-op check before any travel UI is connected.
    bool invalidTravelTargetRejected = !worldmap_authoritative_travel_begin(-1, 0);
    bool travelStepperGuarded = false;
    if (worldMode == NetworkLaunchMode::Host) {
        travelStepperGuarded = worldmap_authoritative_travel_begin(startingWorldMap.x, startingWorldMap.y);
        WorldMapTravelStepResult step = worldmap_authoritative_travel_step();
        travelStepperGuarded = travelStepperGuarded
            && step.status == WorldMapTravelStepStatus::Arrived
            && step.x == startingWorldMap.x
            && step.y == startingWorldMap.y
            && step.gameTime == startingWorldTime
            && !step.dayElapsed;
    } else {
        travelStepperGuarded = !worldmap_authoritative_travel_begin(startingWorldMap.x, startingWorldMap.y)
            && worldmap_authoritative_travel_step().status == WorldMapTravelStepStatus::Invalid;
    }
    GameCommand open;
    open.sequence = CommandSequence { 1 };
    open.playerId = kGuestPlayerId;
    open.actorId = actorId;
    open.expectedPhase = SessionPhase::Exploration;
    open.expectedPhaseRevision = session.phaseRevision();
    GameCommand unapprovedTravel = open;
    unapprovedTravel.payload = SharedModalCommand { SharedModalKind::WorldMap, true };
    AuthoritativeCommandResult travelResult = networkWorldProcessCommand(unapprovedTravel);
    const auto* proposedTravel = travelResult.event.has_value()
        ? std::get_if<SharedModalStateChangedEvent>(&travelResult.event->payload)
        : nullptr;
    bool travelWaitsForConsent = travelResult.result.status == CommandStatus::Accepted
        && proposedTravel != nullptr
        && proposedTravel->actorId == actorId
        && proposedTravel->phase == SessionPhase::Exploration
        && proposedTravel->open
        && session.phase() == SessionPhase::Exploration;
    WorldSnapshot proposalSnapshot;
    travelWaitsForConsent = travelWaitsForConsent
        && networkWorldCaptureSnapshot(EventSequence {}, proposalSnapshot)
        && proposalSnapshot.worldMapTravel.stage == WorldMapTravelStage::Proposed
        && proposalSnapshot.worldMapTravel.proposerActorId == actorId;
    GameCommand approval = unapprovedTravel;
    approval.playerId = kHostPlayerId;
    approval.actorId = session.playerActorId(kHostPlayerId);
    AuthoritativeCommandResult approvedTravel = networkWorldProcessCommand(approval);
    const auto* travelReady = approvedTravel.event.has_value()
        ? std::get_if<SharedModalStateChangedEvent>(&approvedTravel.event->payload)
        : nullptr;
    bool proposerRetainsControl = approvedTravel.result.status == CommandStatus::Accepted
        && travelReady != nullptr
        && travelReady->actorId == actorId
        && travelReady->phase == SessionPhase::Transition
        && networkWorldSharedModalActive();
    GameCommand route = unapprovedTravel;
    route.sequence = CommandSequence { 2 };
    route.expectedPhase = SessionPhase::Transition;
    route.expectedPhaseRevision = session.phaseRevision();
    route.payload = WorldMapRouteCommand { 725, 616, false };
    AuthoritativeCommandResult routed = networkWorldProcessCommand(route);
    const auto* routeEvent = routed.event.has_value()
        ? std::get_if<WorldMapRouteSelectedEvent>(&routed.event->payload)
        : nullptr;
    bool proposerCanRoute = routed.result.status == CommandStatus::Accepted
        && routeEvent != nullptr
        && routeEvent->actorId == actorId
        && selectedWorldMapRoute == std::make_pair(725, 616);
    WorldSnapshot routeSnapshot;
    proposerCanRoute = proposerCanRoute
        && networkWorldCaptureSnapshot(EventSequence {}, routeSnapshot)
        && routeSnapshot.worldMapTravel.stage == WorldMapTravelStage::Approved
        && routeSnapshot.worldMapTravel.proposerActorId == actorId
        && routeSnapshot.worldMapTravel.targetX == 725
        && routeSnapshot.worldMapTravel.targetY == 616;
    proposerCanRoute = proposerCanRoute && networkWorldRunWorldMapRouteStopSmokeTest()
        && worldmap_multiplayer_render_smoke_test()
        && worldMapControllerRulesSmokeTest();
    bool hostTravelProgressRoundTrip = true;
    if (worldMode == NetworkLaunchMode::Host) {
        WorldMapState travelFixture = startingWorldMap;
        travelFixture.x = 1325;
        travelFixture.y = 325; // City terrain, outside the walkmask.
        int savedVaultWater = game_global_vars[GVAR_VAULT_WATER];
        int savedVatsCountdown = game_global_vars[GVAR_VATS_COUNTDOWN];
        int savedMasterCountdown = game_global_vars[GVAR_COUNTDOWN_TO_DESTRUCTION];
        game_global_vars[GVAR_VAULT_WATER] = 1;
        game_global_vars[GVAR_VATS_COUNTDOWN] = 0;
        game_global_vars[GVAR_COUNTDOWN_TO_DESTRUCTION] = 0;
        hostTravelProgressRoundTrip = worldmap_apply_state(travelFixture)
            && worldmap_authoritative_travel_begin(725, 616);
        WorldMapTravelStepResult movement = worldmap_authoritative_travel_step();
        WorldMapState movedMap;
        worldmap_capture_state(movedMap);
        WorldSnapshot movingSnapshot;
        std::vector<std::uint8_t> movingPacket;
        SnapshotDecodeResult decodedMoving;
        hostTravelProgressRoundTrip = hostTravelProgressRoundTrip
            && movement.status == WorldMapTravelStepStatus::Moving
            && movement.x != travelFixture.x
            && movement.gameTime == startingWorldTime
            && !movement.dayElapsed
            && networkWorldCaptureSnapshot(EventSequence {}, movingSnapshot)
            && movingSnapshot.worldMapTravel.progress.active
            && encodeSnapshot(movingSnapshot, movingPacket) == SnapshotError::None;
        if (hostTravelProgressRoundTrip) {
            decodedMoving = decodeSnapshot(movingPacket);
            hostTravelProgressRoundTrip = decodedMoving
                && decodedMoving.snapshot.worldMapTravel.progress.active
                && decodedMoving.snapshot.worldMapTravel.progress.lineIndex
                    == movingSnapshot.worldMapTravel.progress.lineIndex
                && worldmap_apply_state(movedMap)
                && worldmap_apply_travel_progress(decodedMoving.snapshot.worldMapTravel.progress);
            WorldMapTravelProgress restoredProgress;
            worldmap_capture_travel_progress(restoredProgress);
            hostTravelProgressRoundTrip = hostTravelProgressRoundTrip
                && restoredProgress.active
                && restoredProgress.lineIndex == movingSnapshot.worldMapTravel.progress.lineIndex
                && restoredProgress.miles == movingSnapshot.worldMapTravel.progress.miles;
        }
        worldmap_authoritative_travel_cancel();
        hostTravelProgressRoundTrip = worldmap_apply_state(startingWorldMap)
            && hostTravelProgressRoundTrip;
        game_global_vars[GVAR_VAULT_WATER] = savedVaultWater;
        game_global_vars[GVAR_VATS_COUNTDOWN] = savedVatsCountdown;
        game_global_vars[GVAR_COUNTDOWN_TO_DESTRUCTION] = savedMasterCountdown;
    }
    bool hostTravelRules = true;
    if (worldMode == NetworkLaunchMode::Host) {
        int savedVaultWater = game_global_vars[GVAR_VAULT_WATER];
        int savedWaterChip = game_global_vars[GVAR_FIND_WATER_CHIP];
        int savedVatsCountdown = game_global_vars[GVAR_VATS_COUNTDOWN];
        int savedMasterCountdown = game_global_vars[GVAR_COUNTDOWN_TO_DESTRUCTION];
        game_global_vars[GVAR_VAULT_WATER] = 1;
        game_global_vars[GVAR_VATS_COUNTDOWN] = 0;
        game_global_vars[GVAR_COUNTDOWN_TO_DESTRUCTION] = 0;

        Object* remotePlayer = session.entities().findObject(session.playerActorId(kGuestPlayerId));
        int remoteStartingHits = remotePlayer != nullptr ? critter_get_hits(remotePlayer) : 0;
        WorldMapState dayFixture = startingWorldMap;
        dayFixture.x = 1325;
        dayFixture.y = 325; // City terrain; the first two pixels cost no clock time.
        hostTravelRules = remotePlayer != nullptr
            && remoteStartingHits > 1
            && stat_level(remotePlayer, STAT_HEALING_RATE) > 0
            && worldmap_apply_state(dayFixture)
            && worldmap_authoritative_travel_begin(725, 616);
        WorldMapTravelProgress dayProgress;
        worldmap_capture_travel_progress(dayProgress);
        dayProgress.miles = dayProgress.dayLength - 1;
        dayProgress.timeAdder = 0;
        hostTravelRules = hostTravelRules
            && dayProgress.active
            && worldmap_apply_travel_progress(dayProgress);
        if (remotePlayer != nullptr && remoteStartingHits > 1) {
            critter_adjust_hits(remotePlayer, -1);
        }
        // This cell's encounter threshold is 9. Find a deterministic seed
        // whose first 3d6 roll trips it; no map is loaded by the stepper.
        int encounterSeed = 0;
        for (int seed = 1; seed <= 1024; seed++) {
            roll_set_seed(seed);
            int chance = roll_random(1, 6) + roll_random(1, 6) + roll_random(1, 6);
            if (chance < 9) {
                encounterSeed = seed;
                break;
            }
        }
        WorldMapTravelStepResult encounterStep;
        if (hostTravelRules && encounterSeed != 0) {
            roll_set_seed(encounterSeed);
            encounterStep = networkWorldAdvanceWorldMapTravel();
        }
        hostTravelRules = hostTravelRules
            && encounterSeed != 0
            && encounterStep.status == WorldMapTravelStepStatus::Encounter
            && encounterStep.dayElapsed
            && encounterStep.gameTime == startingWorldTime
            && critter_get_hits(remotePlayer) == remoteStartingHits;
        roll_set_seed(-1);
        if (remotePlayer != nullptr) {
            critter_adjust_hits(remotePlayer, remoteStartingHits - critter_get_hits(remotePlayer));
        }
        worldmap_authoritative_travel_cancel();
        hostTravelRules = worldmap_apply_state(startingWorldMap) && hostTravelRules;

        std::vector<QueueEventState> savedQueue;
        bool queueCaptured = queue_capture_state(savedQueue);
        WorldMapState queueFixture = startingWorldMap;
        queueFixture.x = 1200;
        queueFixture.y = 1200; // Coast terrain advances time on the first pixel.
        bool queueStarted = queueCaptured
            && worldmap_apply_state(queueFixture)
            && worldmap_authoritative_travel_begin(725, 616);
        auto* withdrawal = static_cast<WithdrawalEvent*>(mem_malloc(sizeof(WithdrawalEvent)));
        bool queueAdded = false;
        if (withdrawal != nullptr) {
            *withdrawal = WithdrawalEvent { 0, 0, PERK_BUFFOUT_ADDICTION };
            queueAdded = queue_add(1, obj_dude, withdrawal, EVENT_TYPE_WITHDRAWAL) != -1;
            if (!queueAdded) mem_free(withdrawal);
        }
        WorldMapTravelStepResult queueStep;
        if (queueStarted && queueAdded) {
            queueStep = networkWorldAdvanceWorldMapTravel();
        }
        hostTravelRules = hostTravelRules
            && queueStarted && queueAdded
            && queueStep.status == WorldMapTravelStepStatus::QueueInterrupted
            && queueStep.gameTime == startingWorldTime + 1
            && !queueStep.dayElapsed;
        worldmap_authoritative_travel_cancel();
        bool queueRestored = queueCaptured && queue_replace_state(savedQueue);
        set_game_time(startingWorldTime);
        hostTravelRules = queueRestored
            && worldmap_apply_state(startingWorldMap)
            && hostTravelRules;

        game_global_vars[GVAR_VAULT_WATER] = 0;
        game_global_vars[GVAR_FIND_WATER_CHIP] = 0;
        bool worldEventStarted = worldmap_apply_state(queueFixture)
            && worldmap_authoritative_travel_begin(725, 616);
        WorldMapTravelStepResult worldEventStep;
        if (worldEventStarted) {
            worldEventStep = networkWorldAdvanceWorldMapTravel();
        }
        hostTravelRules = hostTravelRules
            && worldEventStarted
            && worldEventStep.status == WorldMapTravelStepStatus::WorldEventPending
            && worldEventStep.x == queueFixture.x
            && worldEventStep.y == queueFixture.y
            && worldEventStep.gameTime == startingWorldTime;
        worldmap_authoritative_travel_cancel();
        hostTravelRules = worldmap_apply_state(startingWorldMap) && hostTravelRules;
        game_global_vars[GVAR_VAULT_WATER] = savedVaultWater;
        game_global_vars[GVAR_FIND_WATER_CHIP] = savedWaterChip;
        game_global_vars[GVAR_VATS_COUNTDOWN] = savedVatsCountdown;
        game_global_vars[GVAR_COUNTDOWN_TO_DESTRUCTION] = savedMasterCountdown;
    }
    // The isolated stepper fixtures above deliberately teleport the world-map
    // marker. They are not a live departure from this consent session.
    worldMapDeparted = false;
    GameCommand unauthorizedRoute = route;
    unauthorizedRoute.playerId = kHostPlayerId;
    unauthorizedRoute.actorId = session.playerActorId(kHostPlayerId);
    unauthorizedRoute.sequence = CommandSequence { 2 };
    AuthoritativeCommandResult unauthorizedRouteResult = networkWorldProcessCommand(unauthorizedRoute);
    bool onlyProposerCanRoute = unauthorizedRouteResult.result.rejection == CommandRejection::InvalidAction
        && !unauthorizedRouteResult.event.has_value()
        && selectedWorldMapRoute == std::make_pair(725, 616);
    route.sequence = CommandSequence { 3 };
    route.payload = WorldMapRouteCommand { -1, -1, true };
    AuthoritativeCommandResult clearedRoute = networkWorldProcessCommand(route);
    bool proposerCanClearRoute = clearedRoute.result.status == CommandStatus::Accepted
        && !selectedWorldMapRoute.has_value();
    GameCommand unauthorizedClose = approval;
    unauthorizedClose.sequence = CommandSequence { 3 };
    unauthorizedClose.expectedPhase = SessionPhase::Transition;
    unauthorizedClose.expectedPhaseRevision = session.phaseRevision();
    unauthorizedClose.payload = SharedModalCommand { SharedModalKind::WorldMap, false };
    AuthoritativeCommandResult unauthorizedResult = networkWorldProcessCommand(unauthorizedClose);
    bool onlyProposerCanClose = unauthorizedResult.result.rejection == CommandRejection::InvalidAction
        && !unauthorizedResult.event.has_value();
    GameCommand cancelTravel = unapprovedTravel;
    cancelTravel.sequence = CommandSequence { 4 };
    cancelTravel.expectedPhase = SessionPhase::Transition;
    cancelTravel.expectedPhaseRevision = session.phaseRevision();
    cancelTravel.payload = SharedModalCommand { SharedModalKind::WorldMap, false };
    AuthoritativeCommandResult canceledTravel = networkWorldProcessCommand(cancelTravel);
    bool proposerCanCancel = canceledTravel.result.status == CommandStatus::Accepted
        && session.phase() == SessionPhase::Exploration
        && !networkWorldSharedModalActive();
    commandProcessor.reset();
    GameCommand hostProposal = approval;
    hostProposal.sequence = CommandSequence { 1 };
    hostProposal.expectedPhase = SessionPhase::Exploration;
    hostProposal.expectedPhaseRevision = session.phaseRevision();
    AuthoritativeCommandResult hostProposed = networkWorldProcessCommand(hostProposal);
    GameCommand guestApproval = unapprovedTravel;
    guestApproval.sequence = CommandSequence { 1 };
    guestApproval.expectedPhaseRevision = session.phaseRevision();
    AuthoritativeCommandResult guestApproved = networkWorldProcessCommand(guestApproval);
    const auto* hostReadyEvent = guestApproved.event.has_value()
        ? std::get_if<SharedModalStateChangedEvent>(&guestApproved.event->payload)
        : nullptr;
    GameCommand hostRoute = hostProposal;
    hostRoute.sequence = CommandSequence { 2 };
    hostRoute.expectedPhase = SessionPhase::Transition;
    hostRoute.expectedPhaseRevision = session.phaseRevision();
    hostRoute.payload = WorldMapRouteCommand { 412, 830, false };
    AuthoritativeCommandResult hostRouted = networkWorldProcessCommand(hostRoute);
    bool hostCanProposeAndRoute = hostProposed.result.status == CommandStatus::Accepted
        && guestApproved.result.status == CommandStatus::Accepted
        && hostReadyEvent != nullptr
        && hostReadyEvent->actorId == hostProposal.actorId
        && hostRouted.result.status == CommandStatus::Accepted
        && selectedWorldMapRoute == std::make_pair(412, 830);
    GameCommand hostClose = hostProposal;
    hostClose.sequence = CommandSequence { 3 };
    hostClose.expectedPhase = SessionPhase::Transition;
    hostClose.expectedPhaseRevision = session.phaseRevision();
    hostClose.payload = SharedModalCommand { SharedModalKind::WorldMap, false };
    AuthoritativeCommandResult hostClosed = networkWorldProcessCommand(hostClose);
    bool routeHasNoWorldEffects = hostClosed.result.status == CommandStatus::Accepted
        && game_time() == startingWorldTime
        && !selectedWorldMapRoute.has_value();
    WorldMapState finalWorldMap;
    worldmap_capture_state(finalWorldMap);
    routeHasNoWorldEffects = routeHasNoWorldEffects
        && finalWorldMap.x == startingWorldMap.x
        && finalWorldMap.y == startingWorldMap.y
        && finalWorldMap.specialEncounters == startingWorldMap.specialEncounters;
    commandProcessor.reset();
    GameCommand takeoverProposal = unapprovedTravel;
    takeoverProposal.expectedPhaseRevision = session.phaseRevision();
    AuthoritativeCommandResult takeoverProposed = networkWorldProcessCommand(takeoverProposal);
    GameCommand takeoverApproval = approval;
    takeoverApproval.expectedPhaseRevision = session.phaseRevision();
    AuthoritativeCommandResult takeoverApproved = networkWorldProcessCommand(takeoverApproval);
    GameCommand takeoverRoute = route;
    takeoverRoute.sequence = CommandSequence { 2 };
    takeoverRoute.expectedPhaseRevision = session.phaseRevision();
    takeoverRoute.payload = WorldMapRouteCommand { 725, 616, false };
    AuthoritativeCommandResult takeoverRouted = networkWorldProcessCommand(takeoverRoute);
    SharedModalStateChangedEvent takeoverEvent {
        session.playerActorId(kHostPlayerId),
        SharedModalKind::WorldMap,
        true,
        SessionPhase::Transition,
        session.phaseRevision(),
    };
    bool takeoverApplied = false;
    if (worldMode == NetworkLaunchMode::Host) {
        networkWorldHostTakeOverWorldMapTravel();
        std::optional<GameEvent> deferredTakeover = networkWorldTakeDeferredEvent();
        const auto* publishedTakeover = deferredTakeover.has_value()
            ? std::get_if<SharedModalStateChangedEvent>(&deferredTakeover->payload)
            : nullptr;
        takeoverApplied = publishedTakeover != nullptr
            && publishedTakeover->actorId == takeoverEvent.actorId
            && publishedTakeover->phase == SessionPhase::Transition;
    } else {
        takeoverApplied = networkWorldApplyPeerSharedModal(takeoverEvent);
    }
    WorldSnapshot takeoverSnapshot;
    takeoverApplied = takeoverApplied
        && networkWorldCaptureSnapshot(EventSequence {}, takeoverSnapshot)
        && takeoverSnapshot.worldMapTravel.stage == WorldMapTravelStage::Approved
        && takeoverSnapshot.worldMapTravel.proposerActorId == actorId
        && takeoverSnapshot.worldMapTravel.controllerActorId == takeoverEvent.actorId
        && takeoverSnapshot.worldMapTravel.targetX == 725
        && takeoverSnapshot.worldMapTravel.targetY == 616;
    GameCommand formerControllerRoute = takeoverRoute;
    formerControllerRoute.sequence = CommandSequence { 3 };
    AuthoritativeCommandResult formerControllerRejected = networkWorldProcessCommand(formerControllerRoute);
    GameCommand newControllerRoute = takeoverApproval;
    newControllerRoute.sequence = CommandSequence { 2 };
    newControllerRoute.expectedPhase = SessionPhase::Transition;
    newControllerRoute.expectedPhaseRevision = session.phaseRevision();
    newControllerRoute.payload = WorldMapRouteCommand { 700, 600, false };
    AuthoritativeCommandResult newControllerAccepted = networkWorldProcessCommand(newControllerRoute);
    GameCommand takeoverClose = newControllerRoute;
    takeoverClose.sequence = CommandSequence { 3 };
    takeoverClose.payload = SharedModalCommand { SharedModalKind::WorldMap, false };
    AuthoritativeCommandResult takeoverClosed = networkWorldProcessCommand(takeoverClose);
    bool hostTakeoverWorks = takeoverProposed.result.status == CommandStatus::Accepted
        && takeoverApproved.result.status == CommandStatus::Accepted
        && takeoverRouted.result.status == CommandStatus::Accepted
        && takeoverApplied
        && formerControllerRejected.result.rejection == CommandRejection::InvalidAction
        && newControllerAccepted.result.status == CommandStatus::Accepted
        && takeoverClosed.result.status == CommandStatus::Accepted
        && session.phase() == SessionPhase::Exploration
        && !selectedWorldMapRoute.has_value();
    commandProcessor.reset();
    open.expectedPhaseRevision = session.phaseRevision();
    open.payload = SharedModalCommand { SharedModalKind::Dialogue, true };
    AuthoritativeCommandResult opened = networkWorldProcessCommand(open);
    const auto* openedEvent = opened.event.has_value()
        ? std::get_if<SharedModalStateChangedEvent>(&opened.event->payload)
        : nullptr;
    bool openPassed = opened.result.status == CommandStatus::Accepted
        && openedEvent != nullptr
        && openedEvent->open
        && openedEvent->phase == SessionPhase::Dialogue
        && openedEvent->phaseRevision == session.phaseRevision()
        && networkWorldSharedModalActive();

    GameCommand blocked = open;
    blocked.sequence = CommandSequence { 2 };
    blocked.expectedPhase = SessionPhase::Dialogue;
    blocked.expectedPhaseRevision = session.phaseRevision();
    blocked.payload = MoveCommand { 1, 0, false };
    AuthoritativeCommandResult blockedResult = networkWorldProcessCommand(blocked);
    bool blockPassed = blockedResult.result.rejection == CommandRejection::WrongPhase
        && !blockedResult.event.has_value();

    GameCommand close = open;
    close.sequence = CommandSequence { 3 };
    close.expectedPhase = SessionPhase::Dialogue;
    close.expectedPhaseRevision = session.phaseRevision();
    close.payload = SharedModalCommand { SharedModalKind::Dialogue, false };
    AuthoritativeCommandResult closed = networkWorldProcessCommand(close);
    const auto* closedEvent = closed.event.has_value()
        ? std::get_if<SharedModalStateChangedEvent>(&closed.event->payload)
        : nullptr;
    bool closePassed = closed.result.status == CommandStatus::Accepted
        && closedEvent != nullptr
        && !closedEvent->open
        && closedEvent->phase == SessionPhase::Exploration
        && closedEvent->phaseRevision == session.phaseRevision()
        && !networkWorldSharedModalActive();

    commandProcessor.reset();
    return worldMapExitProposes && invalidTravelTargetRejected && travelStepperGuarded
        && hostTravelProgressRoundTrip && hostTravelRules
        && travelWaitsForConsent && proposerRetainsControl && proposerCanRoute
        && onlyProposerCanRoute && proposerCanClearRoute && onlyProposerCanClose
        && proposerCanCancel && hostCanProposeAndRoute && routeHasNoWorldEffects
        && hostTakeoverWorks
        && openPassed && blockPassed && closePassed;
}

bool networkWorldBeginLocalLoot(Object* target)
{
    if (!session.isActive()
        || obj_dude == nullptr
        || target == nullptr
        || isPlayerActor(target)
        || (FID_TYPE(target->fid) != OBJ_TYPE_CRITTER
            && (FID_TYPE(target->fid) != OBJ_TYPE_ITEM || target->owner != nullptr
                || item_get_type(target) != ITEM_TYPE_CONTAINER))
        || obj_dude->elevation != target->elevation) {
        return false;
    }
    activeLootTargets[obj_dude] = target;
    return scripts_request_loot_container(obj_dude, target) == 0;
}

bool networkWorldSetLocalLootTarget(Object* target)
{
    if (!session.isActive()
        || obj_dude == nullptr
        || target == nullptr
        || isPlayerActor(target)
        || FID_TYPE(target->fid) != OBJ_TYPE_CRITTER
        || obj_dude->elevation != target->elevation) {
        return false;
    }
    activeLootTargets[obj_dude] = target;
    return true;
}

bool networkWorldIsTheftTarget(const Object* actor, const Object* target)
{
    auto found = theftAccess.find(const_cast<Object*>(actor));
    return session.isActive() && session.phase() == SessionPhase::Exploration
        && found != theftAccess.end() && found->second.target == target
        && session.entities().findEntity(target).has_value()
        && session.entities().findEntity(actor).has_value()
        && actor->tile >= 0 && target->tile >= 0 && actor->elevation == target->elevation
        && obj_dist(const_cast<Object*>(actor), const_cast<Object*>(target)) <= 1
        && critter_is_active(const_cast<Object*>(target));
}

bool networkWorldHasTheftAccess(const Object* actor)
{
    return session.isActive() && theftAccess.count(const_cast<Object*>(actor)) != 0;
}

bool networkWorldIsLocalInventoryTransfer(Object* source, Object* destination)
{
    Object* localActor = localPlayerActor();
    if (!session.isActive() || localActor == nullptr || source == nullptr || destination == nullptr) {
        return false;
    }
    Object* sourceTop = topEnvironmentOrSelf(source);
    Object* destinationTop = topEnvironmentOrSelf(destination);
    if (sourceTop == localActor && destinationTop == localActor) {
        // Host scripts already run authoritatively. Only intercept native UI
        // moves here; scripts may rearrange inventory during other phases.
        return inven_network_inventory_window_is_active();
    }
    auto activeLoot = activeLootTargets.find(localActor);
    if (activeLoot == activeLootTargets.end()) {
        return false;
    }
    return (sourceTop == localActor && destinationTop == activeLoot->second)
        || (destinationTop == localActor && sourceTop == activeLoot->second);
}

bool networkWorldIsLocalItemDrop(Object* source, Object* item)
{
    return session.isActive()
        && obj_dude != nullptr
        && source != nullptr
        && item != nullptr
        && topEnvironmentOrSelf(source) == obj_dude
        && item_count(source, item) > 0;
}

bool networkWorldItemUseInProgress()
{
    return itemUseInProgress;
}

void networkWorldHandleObjectDestroyed(Object* object)
{
    if (!session.isActive() || object == nullptr) {
        return;
    }
    for (auto it = theftAccess.begin(); it != theftAccess.end();) {
        if (it->first == object || it->second.target == object) {
            activeLootTargets.erase(it->first);
            it = theftAccess.erase(it);
        } else ++it;
    }
    for (auto it = activeLootTargets.begin(); it != activeLootTargets.end();) {
        if (it->first == object || it->second == object) it = activeLootTargets.erase(it);
        else ++it;
    }
    std::optional<EntityId> entityId = session.entities().findEntity(object);
    if (!entityId.has_value()) {
        return;
    }
    if (npcBarterState.has_value() && (npcBarterState->buyerId == *entityId || npcBarterState->sellerId == *entityId)) {
        npcBarterState.reset();
        pendingScriptedNpcBarter.reset();
    }
    session.entities().unregisterEntity(*entityId);
    auto remove = [object](auto& entries) {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [object](const auto& entry) {
            return entry.second == object;
        }), entries.end());
    };
    remove(worldItems);
    remove(worldCritters);
    remove(worldScenery);
    remove(worldDoors);
    remove(worldExitGrids);
}

void networkWorldHandleItemReplacement(Object* removed, Object* replacement)
{
    if (!session.isActive() || removed == nullptr || replacement == nullptr || removed == replacement) {
        return;
    }
    std::optional<EntityId> removedId = session.entities().findEntity(removed);
    if (!removedId.has_value()) {
        return;
    }
    std::optional<EntityId> replacementId = session.entities().findEntity(replacement);
    if (replacementId.has_value()) {
        session.entities().unregisterEntity(*removedId);
        worldItems.erase(std::remove_if(worldItems.begin(), worldItems.end(), [&](const auto& entry) {
            return entry.first == *removedId;
        }), worldItems.end());
    } else if (session.entities().rebindObject(*removedId, replacement) == EntityRegistryError::None) {
        for (auto& entry : worldItems) {
            if (entry.first == *removedId) {
                entry.second = replacement;
                break;
            }
        }
    }
}

void networkWorldHandleItemSplit(Object* original, Object* remainder)
{
    if (!session.isActive() || original == nullptr || remainder == nullptr) {
        return;
    }
    std::optional<EntityId> originalId = session.entities().findEntity(original);
    if (!originalId.has_value() || session.entities().findEntity(remainder).has_value()) {
        return;
    }

    EntityId remainderId;
    if (isValid(expectedSplitEntityId)) {
        if (session.entities().restoreObject(expectedSplitEntityId, remainder) != EntityRegistryError::None) {
            return;
        }
        remainderId = expectedSplitEntityId;
    } else if (worldMode == NetworkLaunchMode::Host) {
        EntityRegistrationResult registration = registerItem(remainder);
        if (!registration) {
            return;
        }
        remainderId = registration.entityId;
    } else {
        return;
    }
    trackWorldItem(remainderId, remainder);
    lastSplitEntityId = remainderId;
}

bool networkWorldDescribeItem(const Object* item, ItemDescriptor& descriptor)
{
    return session.isActive() && describeItem(item, descriptor);
}

std::optional<EntityId> networkWorldEnsureItemRegistered(Object* item)
{
    if (!session.isActive() || item == nullptr) {
        return std::nullopt;
    }
    std::optional<EntityId> existing = session.entities().findEntity(item);
    if (existing.has_value()) {
        return existing;
    }
    if (item->data.inventory.length != 0) {
        return std::nullopt;
    }
    EntityRegistrationResult registration = registerItem(item);
    return registration ? std::optional<EntityId>(registration.entityId) : std::nullopt;
}

void networkWorldResetLastItemSplit()
{
    lastSplitEntityId = {};
}

EntityId networkWorldTakeLastItemSplit()
{
    EntityId entityId = lastSplitEntityId;
    lastSplitEntityId = {};
    return entityId;
}

bool networkWorldApplyInventoryTransfer(const InventoryTransferredEvent& transfer, bool reverse)
{
    if (!session.isActive()) {
        return false;
    }
    EntityId sourceId = reverse ? transfer.destinationId : transfer.sourceId;
    EntityId destinationId = reverse ? transfer.sourceId : transfer.destinationId;
    Object* source = session.entities().findObject(sourceId);
    Object* destination = session.entities().findObject(destinationId);
    Object* item = session.entities().findObject(transfer.itemId);
    if (source == nullptr || destination == nullptr || source == destination) {
        return false;
    }
    // A completed event may already have registered its remainder. Recognize
    // replay before requiring a fresh ID for an event that still needs applying.
    if (item != nullptr && item->owner == destination) {
        ItemDescriptor descriptor;
        if (!describeItem(item, descriptor)
            || !itemDescriptorsEqual(descriptor, transfer.itemDescriptor)) return false;
        inven_refresh_loot_window();
        inven_refresh_inventory_window();
        return true;
    }
    if ((!reverse && !remainderIdAvailable(transfer.remainderItemId))
        || (item != nullptr && inventoryTransferWouldCycle(destination, item))) return false;
    if (item == nullptr && !reverse) {
        Inventory* inventory = &source->data.inventory;
        bool descriptorMatchFound = false;
        for (int index = 0; index < inventory->length; index++) {
            Object* candidate = inventory->items[index].item;
            ItemDescriptor candidateDescriptor;
            if (describeItem(candidate, candidateDescriptor)
                && itemDescriptorsEqual(candidateDescriptor, transfer.itemDescriptor)) {
                if (descriptorMatchFound
                    || session.entities().findEntity(candidate).has_value()
                    || inventory->items[index].quantity != static_cast<int>(transfer.sourceQuantity)) {
                    return false;
                }
                descriptorMatchFound = true;
                item = candidate;
            }
        }
        if (item != nullptr && inventoryTransferWouldCycle(destination, item)) return false;
        bool created = false;
        if (item == nullptr) {
            item = createItem(transfer.itemDescriptor);
            if (item == nullptr
                || item_add_force(source, item, static_cast<int>(transfer.sourceQuantity)) != 0) {
                if (item != nullptr) {
                    obj_erase_object(item, nullptr);
                }
                return false;
            }
            // Native inventory adoption leaves a newly created floating node
            // behind. Remove it before transfer or shutdown traverses both roots.
            if (obj_disconnect(item, nullptr) != 0) {
                item_remove_mult(source, item, static_cast<int>(transfer.sourceQuantity));
                obj_erase_object(item, nullptr);
                return false;
            }
            created = true;
        }
        if (session.entities().restoreObject(transfer.itemId, item) != EntityRegistryError::None) {
            if (created) {
                item_remove_mult(source, item, static_cast<int>(transfer.sourceQuantity));
                obj_erase_object(item, nullptr);
            }
            return false;
        }
        trackWorldItem(transfer.itemId, item);
    }
    if (item == nullptr) {
        return false;
    }
    ItemDescriptor actualDescriptor;
    if (!describeItem(item, actualDescriptor)
        || actualDescriptor.pid != transfer.itemDescriptor.pid
        || actualDescriptor.extendedFlags != transfer.itemDescriptor.extendedFlags
        || actualDescriptor.data0 != transfer.itemDescriptor.data0
        || actualDescriptor.data1 != transfer.itemDescriptor.data1) {
        return false;
    }
    if (item->owner == destination) {
        inven_refresh_loot_window();
        inven_refresh_inventory_window();
        return true;
    }
    if (item_count(source, item) != static_cast<int>(transfer.sourceQuantity)) {
        return false;
    }
    EntityId expectedRemainder = reverse ? EntityId {} : transfer.remainderItemId;
    bool applied = applyInventoryTransfer(source,
        destination,
        item,
        transfer.quantity,
        true,
        expectedRemainder,
        !reverse);
    if (applied) {
        inven_refresh_loot_window();
        inven_refresh_inventory_window();
    }
    return applied;
}

bool networkWorldApplyCapsDistribution(
    const CapsDistributedEvent& distribution)
{
    if (!networkWorldReplicaSessionActive()
        || session.entities().findObject(distribution.actorId) == nullptr
        || session.entities().findObject(distribution.sourceId) == nullptr
        || distribution.shares.size() < 2) return false;
    std::uint64_t sum = 0;
    for (const PlayerCapShare& share : distribution.shares) {
        if (session.playerActorId(share.playerId) != share.actorId
            || session.entities().findObject(share.actorId) == nullptr) {
            return false;
        }
        sum += share.caps;
    }
    // The immediately following authoritative checkpoint applies all cap
    // inventory mutations together, avoiding partial replica-side updates.
    return sum == distribution.caps;
}

bool networkWorldApplyItemDrop(const ItemDroppedEvent& drop)
{
    if (!session.isActive()) {
        return false;
    }

    PlayerCharacterState* player = session.players().findByActor(drop.actorId);
    Object* actor = player != nullptr ? session.entities().findObject(player->actorId) : nullptr;
    Object* source = session.entities().findObject(drop.sourceId);
    Object* item = session.entities().findObject(drop.itemId);
    if (player == nullptr
        || actor == nullptr
        || source == nullptr
        || topEnvironmentOrSelf(source) != actor
        || !hexGridTileIsValid(drop.tile)
        || !elevationIsValid(drop.elevation)) {
        return false;
    }

    if (item != nullptr && item->owner == nullptr
        && item->tile == drop.tile && item->elevation == drop.elevation) {
        ItemDescriptor descriptor;
        if (!describeItem(item, descriptor)
            || !itemDescriptorsEqual(descriptor, drop.itemDescriptor)) return false;
        inven_refresh_inventory_window();
        inven_refresh_loot_window();
        return true;
    }
    if (!remainderIdAvailable(drop.remainderItemId)) return false;

    if (item == nullptr) {
        Inventory* inventory = &source->data.inventory;
        bool descriptorMatchFound = false;
        for (int index = 0; index < inventory->length; index++) {
            Object* candidate = inventory->items[index].item;
            ItemDescriptor candidateDescriptor;
            bool descriptorMatches = describeItem(candidate, candidateDescriptor)
                && (drop.quantity > 1
                        ? candidateDescriptor.pid == PROTO_ID_MONEY
                        : itemDescriptorsEqual(candidateDescriptor, drop.itemDescriptor));
            if (descriptorMatches) {
                if (descriptorMatchFound
                    || session.entities().findEntity(candidate).has_value()
                    || inventory->items[index].quantity != static_cast<int>(drop.sourceQuantity)) {
                    return false;
                }
                descriptorMatchFound = true;
                item = candidate;
            }
        }

        bool created = false;
        if (item == nullptr) {
            item = createItem(drop.itemDescriptor);
            if (item == nullptr
                || item_add_force(source, item, static_cast<int>(drop.sourceQuantity)) != 0) {
                if (item != nullptr) {
                    obj_erase_object(item, nullptr);
                }
                return false;
            }
            if (obj_disconnect(item, nullptr) != 0) {
                item_remove_mult(source, item, static_cast<int>(drop.sourceQuantity));
                obj_erase_object(item, nullptr);
                return false;
            }
            created = true;
        }
        if (session.entities().restoreObject(drop.itemId, item) != EntityRegistryError::None) {
            if (created) {
                item_remove_mult(source, item, static_cast<int>(drop.sourceQuantity));
                obj_erase_object(item, nullptr);
            }
            return false;
        }
        trackWorldItem(drop.itemId, item);
    }

    ItemDescriptor actualDescriptor;
    if (!describeItem(item, actualDescriptor)
        || (drop.quantity == 1 && !itemDescriptorsEqual(actualDescriptor, drop.itemDescriptor))
        || (drop.quantity > 1 && actualDescriptor.pid != PROTO_ID_MONEY)) {
        return false;
    }
    if (item->owner == nullptr && item->tile == drop.tile && item->elevation == drop.elevation) {
        if (!itemDescriptorsEqual(actualDescriptor, drop.itemDescriptor)) {
            return false;
        }
        inven_refresh_inventory_window();
        inven_refresh_loot_window();
        return true;
    }
    if (item_count(source, item) != static_cast<int>(drop.sourceQuantity)
        || !applyItemDrop(source, item, drop.quantity, drop.remainderItemId, true)
        || !describeItem(item, actualDescriptor)
        || !itemDescriptorsEqual(actualDescriptor, drop.itemDescriptor)) {
        return false;
    }

    if (item->tile != drop.tile || item->elevation != drop.elevation) {
        Rect dirtyRect;
        if (obj_move_to_tile(item, drop.tile, drop.elevation, &dirtyRect) == -1) {
            return false;
        }
        tile_refresh_rect(&dirtyRect, drop.elevation);
    }
    inven_refresh_inventory_window();
    inven_refresh_loot_window();
    intface_redraw();
    return true;
}

bool networkWorldApplyLocalItemDrop(Object* source, Object* item, std::uint32_t quantity)
{
    return session.isActive()
        && networkWorldIsLocalItemDrop(source, item)
        && applyItemDrop(source, item, quantity);
}

bool networkWorldBeginLocalPickup(Object* target)
{
    if (!session.isActive() || obj_dude == nullptr || target == nullptr) {
        return false;
    }

    PlayerId localPlayerId = session.entities().findObject(session.playerActorId(kHostPlayerId)) == obj_dude
        ? kHostPlayerId
        : kGuestPlayerId;
    PlayerCharacterState* player = session.players().find(localPlayerId);
    if (player == nullptr) {
        return false;
    }

    ScopedActingPlayerContext actingPlayer(*player, obj_dude);
    return beginPickup(obj_dude, target);
}

void networkWorldFinishPickup(Object* target, bool succeeded)
{
    if (!session.isActive() || target == nullptr) {
        return;
    }
    std::optional<EntityId> targetId = session.entities().findEntity(target);
    if (!targetId.has_value()) {
        return;
    }
    reservedPickupTargets.erase(*targetId);
    auto pending = pendingPickups.find(*targetId);
    if (pending == pendingPickups.end()) {
        return;
    }

    ItemPickupCompletedEvent completion;
    completion.actorId = pending->second.actorId;
    completion.targetId = *targetId;
    Object* actor = session.entities().findObject(completion.actorId);
    int quantity = actor != nullptr ? item_count(actor, target) : 0;
    completion.succeeded = succeeded
        && actor != nullptr
        && target->owner == actor
        && quantity > 0
        && describeItem(target, completion.itemDescriptor);
    if (completion.succeeded) {
        completion.quantity = static_cast<std::uint32_t>(quantity);
    } else {
        completion.itemDescriptor = {};
    }

    deferredEvents.push_back(GameEvent {
        {},
        pending->second.commandSequence,
        completion,
    });
    pendingPickups.erase(pending);
}

AuthoritativeCommandResult networkWorldProcessCommand(const GameCommand& command)
{
    std::vector<std::uint8_t> path;
    int startingTile = -1;
    if (const auto* move = std::get_if<MoveCommand>(&command.payload)) {
        Object* actor = session.entities().findObject(command.actorId);
        if (actor != nullptr) {
            std::array<unsigned char, kMaximumMovementPathLength> rotations;
            int pathLength = make_path(actor, actor->tile, move->destinationTile, rotations.data(), 1);
            if (pathLength > 0 && pathLength <= kAnimationMaximumPathLength) {
                startingTile = actor->tile;
                path.assign(rotations.begin(), rotations.begin() + pathLength);
            }
        }
    }

    const auto* combatPickup = std::get_if<PickupCommand>(&command.payload);
    bool tracksCombatPickup = combatPickup != nullptr && combatPickup->turnRevision != 0
        && pendingPickups.count(combatPickup->targetId) == 0;
    if (tracksCombatPickup) pendingPickups[combatPickup->targetId] = PendingPickup { command.actorId, command.sequence };
    AuthoritativeCommandResult result = commandProcessor.process(command, session, commandExecutor);
    if (tracksCombatPickup) pendingPickups.erase(combatPickup->targetId);
    if (result.result.status == CommandStatus::Accepted && !result.replayed
        && result.event.has_value()) {
        if (std::holds_alternative<CombatRequestedEvent>(result.event->payload)) {
            if (pendingCombatStart.has_value()) pendingCombatStart->causedBy = command.sequence;
        } else if (std::holds_alternative<DialogueRequestedEvent>(result.event->payload)) {
            if (pendingTalk.has_value()) pendingTalk->causedBy = command.sequence;
            dialogueCause = command.sequence;
        } else if (const auto* modal = std::get_if<SharedModalStateChangedEvent>(
                       &result.event->payload);
            modal != nullptr && modal->kind == SharedModalKind::Dialogue) {
            if (modal->open) {
                dialogueCause = command.sequence;
                dialogueVotes.clear();
                dialoguePresentation.reset();
            } else {
                dialogueVotes.clear();
                dialoguePresentation.reset();
                dialogueCause = {};
            }
        }
    }
    if (result.result.status == CommandStatus::Accepted
        && !result.replayed
        && result.event.has_value()
        && std::holds_alternative<SharedModalStateChangedEvent>(result.event->payload)
        && std::get<SharedModalStateChangedEvent>(result.event->payload).kind == SharedModalKind::WorldMap
        && std::get<SharedModalStateChangedEvent>(result.event->payload).open
        && std::get<SharedModalStateChangedEvent>(result.event->payload).phase == SessionPhase::Exploration
        && pendingWorldMapProposal.has_value()
        && pendingWorldMapProposal->proposalSequence.value == 0) {
        pendingWorldMapProposal->proposalSequence = command.sequence;
    }
    if (result.event.has_value()) {
        if (auto* movement = std::get_if<ActorMovementStartedEvent>(&result.event->payload)) {
            movement->startingTile = startingTile;
            movement->path = std::move(path);
        } else if (const auto* pickup = std::get_if<ItemPickupStartedEvent>(&result.event->payload);
            pickup != nullptr && !result.replayed
                && (combatPickup == nullptr || combatPickup->turnRevision == 0)) {
            pendingPickups[pickup->targetId] = PendingPickup {
                pickup->actorId,
                result.event->causedBy,
            };
        }
    }
    return result;
}

std::optional<GameEvent> networkWorldTakeDeferredEvent()
{
    if (deferredEvents.empty()) {
        return std::nullopt;
    }
    GameEvent event = std::move(deferredEvents.front());
    deferredEvents.pop_front();
    return event;
}

bool networkWorldTakePendingTalk(EntityId& actorId, EntityId& targetId)
{
    if (worldMode != NetworkLaunchMode::Host || !pendingTalk.has_value()) return false;
    actorId = pendingTalk->actorId;
    targetId = pendingTalk->targetId;
    pendingTalk.reset();
    return true;
}

bool networkWorldPublishDialogue(const std::string& reply,
    const std::vector<std::string>& options)
{
    if (worldMode != NetworkLaunchMode::Host || !activeSharedModal.has_value()
        || activeSharedModal->kind != SharedModalKind::Dialogue
        || session.phase() != SessionPhase::Dialogue
        || dialogueCause.value == 0 || reply.empty() || reply.size() > 899
        || options.empty() || options.size() > kMaximumDialogueOptions) return false;
    for (const std::string& option : options) {
        if (option.empty() || option.size() > 899) return false;
    }
    Object* target = dialog_target;
    std::optional<EntityId> targetId = session.entities().findEntity(target);
    PlayerCharacterState* talker = session.players().findByActor(activeSharedModal->actorId);
    if (!targetId.has_value() || talker == nullptr) return false;
    DialoguePresentationEvent presentation {
        activeSharedModal->actorId, *targetId, nextDialogueRevision++,
        static_cast<std::uint8_t>(DialogueVotingPolicy::MajorityStatsRandomTie),
        reply, options,
    };
    if (!dialogueVotes.begin(presentation.revision, talker->id,
            kHostPlayerId, session.players().playerIds(),
            static_cast<std::uint8_t>(options.size()),
            DialogueVotingPolicy::MajorityStatsRandomTie,
            combatClockMilliseconds() + 60000)) {
        return false;
    }
    for (const DialogueBallot& ballot : dialogueVotes.ballots()) {
        Object* voter = networkWorldPlayerActor(ballot.playerId);
        networkWorldDialogueSetConnected(ballot.playerId, true);
        PlayerCharacterState* voterState = session.players().find(ballot.playerId);
        if (voter == nullptr || voterState == nullptr) return false;
        ScopedActingPlayerContext acting(*voterState, voter);
        if (!dialogueVotes.setTieBreakStats(ballot.playerId,
                stat_level(voter, STAT_CHARISMA),
                stat_level(voter, STAT_INTELLIGENCE))) return false;
    }
    dialoguePresentation = presentation;
    deferredEvents.push_back(GameEvent { {}, dialogueCause, std::move(presentation) });
    return true;
}

std::optional<std::uint8_t> networkWorldResolveDialogue()
{
    if (worldMode != NetworkLaunchMode::Host || !dialogueVotes.active() || npcBarterState.has_value()) return std::nullopt;
    std::uint64_t now = combatClockMilliseconds();
    // Start the voting timeout when a player participates, keeping the other
    // players' response window after a long idle conversation.
    if (std::none_of(dialogueVotes.ballots().begin(), dialogueVotes.ballots().end(),
            [](const DialogueBallot& ballot) { return ballot.option.has_value(); })) {
        dialogueVotes.deferDeadlineUntil(now + 60000);
    }
    std::optional<std::uint8_t> selected = dialogueVotes.resolve(now);
    if (!selected.has_value() && dialogueVotes.needsRandomTie()) {
        selected = dialogueVotes.resolve(now,
            static_cast<std::uint32_t>(roll_random(0,
                static_cast<int>(dialogueVotes.randomTieOptionCount() - 1))));
    }
    return selected;
}

void networkWorldConsumeDialogueChoice()
{
    dialogueVotes.clear();
    dialoguePresentation.reset();
}

const DialoguePresentationEvent* networkWorldDialoguePresentation()
{
    return dialoguePresentation.has_value() ? &*dialoguePresentation : nullptr;
}

const std::vector<DialogueBallot>& networkWorldDialogueBallots()
{
    return dialogueVotes.ballots();
}

std::string networkWorldDialoguePlayerName(PlayerId playerId)
{
    const PlayerCharacterState* player = session.players().find(playerId);
    return player != nullptr ? player->name : std::string();
}

void networkWorldDialogueSetConnected(PlayerId playerId, bool connected)
{
    if (worldMode == NetworkLaunchMode::Host) {
        Object* actor = networkWorldPlayerActor(playerId);
        dialogueVotes.setConnected(playerId, connected && actor != nullptr
            && !critter_is_dead(actor)
            && (actor->data.critter.combat.results & DAM_KNOCKED_OUT) == 0);
    }
}

ScopedPlayerFeedback::ScopedPlayerFeedback(Object* actor)
    : previous(playerFeedbackActor)
{
    playerFeedbackActor = actor;
}

ScopedPlayerFeedback::~ScopedPlayerFeedback()
{
    playerFeedbackActor = previous;
}

bool networkWorldCapturingPlayerFeedback(const Object* actor)
{
    return worldMode == NetworkLaunchMode::Host && session.isActive()
        && actor != nullptr && actor == playerFeedbackActor
        && playerStateForActor(actor) != nullptr;
}

bool networkWorldRoutePlayerFeedback(const char* text)
{
    if (worldMode != NetworkLaunchMode::Host || !session.isActive()
        || playerFeedbackActor == nullptr || text == nullptr || text[0] == '\0') return false;
    auto* player = playerStateForActor(playerFeedbackActor);
    // Local bindings can temporarily select a guest for legacy inventory and
    // dialogue helpers. The physical host monitor still belongs to the host.
    if (player == nullptr || player->id == kHostPlayerId) return false;
    deferredEvents.push_back(GameEvent { {}, CommandSequence { UINT64_MAX },
        PlayerFeedbackEvent { player->actorId, std::string(text).substr(0, 512) } });
    return true;
}

bool networkWorldApplyPeerPlayerFeedback(const PlayerFeedbackEvent& event)
{
    if (worldMode != NetworkLaunchMode::Join || !session.isActive()
        || session.players().findByActor(event.actorId) == nullptr
        || event.text.empty() || event.text.size() > 512
        || event.text.find('\0') != std::string::npos) return false;
    if (localPlayerState() != nullptr && localPlayerState()->actorId == event.actorId) {
        std::string text = event.text;
        display_print(text.data());
    }
    return true;
}

void networkWorldRecordQuestActivity(int globalVar, int value)
{
    if (worldMode != NetworkLaunchMode::Host || !session.isActive()) return;
    int messageId = pipboy_quest_message_id_for_global(globalVar);
    if (messageId == 0) return;
    publishSharedActivity(SharedActivityKind::Quest, messageId, value,
        value > 1 ? "Quest progressed" : value > 0 ? "Quest updated" : "Quest status changed");
}

void networkWorldObserveWorldMapDiscoveries()
{
    if (worldMode != NetworkLaunchMode::Host || !session.isActive()) return;
    WorldMapState map;
    worldmap_capture_state(map);
    std::uint32_t visits = static_cast<std::uint32_t>(map.firstVisits);
    std::uint32_t newVisits = visits & ~observedFirstVisits;
    observedFirstVisits = visits;
    for (int town = 0; town < 12; town++) {
        if ((newVisits & (1u << town)) != 0) {
            publishSharedActivity(SharedActivityKind::Discovery, town, 1,
                "New location discovered");
        }
    }
}

bool networkWorldApplyPeerSharedActivity(const SharedActivityPublishedEvent& event)
{
    if (worldMode != NetworkLaunchMode::Join || !session.isActive()
        || event.entry.id == 0 || event.entry.sourceName.empty()
        || event.entry.sourceName.size() > 32
        || event.entry.text.empty() || event.entry.text.size() > 160) return false;
    if (!sharedActivity.empty() && event.entry.id <= sharedActivity.back().id) {
        if (event.entry.id < sharedActivity.front().id) {
            return true; // This already fell outside the bounded replay window.
        }
        auto found = std::find_if(sharedActivity.begin(), sharedActivity.end(),
            [&](const SharedActivityEntry& entry) {
                return entry.id == event.entry.id;
            });
        return found != sharedActivity.end()
            && found->sourceId == event.entry.sourceId
            && found->sourceName == event.entry.sourceName
            && found->kind == event.entry.kind
            && found->subject == event.entry.subject
            && found->value == event.entry.value
            && found->text == event.entry.text;
    }
    sharedActivity.push_back(event.entry);
    if (sharedActivity.size() > 64) sharedActivity.pop_front();
    nextSharedActivityId = event.entry.id + 1;
    return true;
}

std::vector<SharedActivityEntry> networkWorldSharedActivity()
{
    return { sharedActivity.begin(), sharedActivity.end() };
}

std::vector<EntityId> networkWorldDialogueSmokeTargets()
{
    std::vector<EntityId> targets;
    Object* host = networkWorldPlayerActor(kHostPlayerId);
    if (!session.isActive() || host == nullptr) return targets;
    for (const auto& entry : worldCritters) {
        Object* critter = entry.second;
        if (critter != nullptr && critter->sid != -1
            && critter_get_hits(critter) > 0 && critter->tile >= 0) {
            targets.push_back(entry.first);
        }
    }
    std::sort(targets.begin(), targets.end(), [&](EntityId a, EntityId b) {
        Object* left = session.entities().findObject(a);
        Object* right = session.entities().findObject(b);
        int leftDistance = left != nullptr ? obj_dist(host, left) : 100000;
        int rightDistance = right != nullptr ? obj_dist(host, right) : 100000;
        return leftDistance != rightDistance
            ? leftDistance < rightDistance : a.value < b.value;
    });
    return targets;
}

bool networkWorldMovePartyNearDialogueTarget(EntityId targetId)
{
    if (worldMode != NetworkLaunchMode::Host
        || session.phase() != SessionPhase::Exploration) return false;
    Object* target = session.entities().findObject(targetId);
    Object* host = networkWorldPlayerActor(kHostPlayerId);
    Object* guest = networkWorldPlayerActor(kGuestPlayerId);
    if (target == nullptr || host == nullptr || guest == nullptr) return false;
    std::vector<int> tiles;
    for (int distance = 1; distance <= 2; distance++) {
        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            int tile = tile_num_in_direction(target->tile, rotation, distance);
            if (hexGridTileIsValid(tile)
                && obj_blocking_at(target, tile, target->elevation) == nullptr) {
                tiles.push_back(tile);
            }
        }
    }
    for (int first : tiles) {
        if (obj_move_to_tile(host, first, target->elevation, nullptr) == -1) continue;
        for (int second : tiles) {
            if (second != first
                && obj_blocking_at(guest, second, target->elevation) == nullptr
                && obj_move_to_tile(guest, second, target->elevation, nullptr) == 0) {
                return true;
            }
        }
    }
    return false;
}

// Run after the native dialogue and its acting-player scopes have unwound.
// Loading inside gdialog_exit would destroy its script and talker objects.
void networkWorldProcessDialogueMapTransition()
{
    if (worldMode != NetworkLaunchMode::Host || !session.isActive()
        || session.phase() != SessionPhase::Exploration
        || !pendingDialogueMapTransition.has_value()) return;
    auto pending = *pendingDialogueMapTransition;
    pendingDialogueMapTransition.reset();
    const auto* player = session.players().findByActor(pending.actorId);
    if (player == nullptr) return;
    PlayerId proposer = player->id;
    if (session.transitionTo(SessionPhase::Transition) != LocalSessionError::None
        || !loadSharedMap(pending.destination.map)) return;
    Object* host = networkWorldPlayerActor(kHostPlayerId);
    int tile = pending.destination.tile;
    int elevation = pending.destination.elevation;
    int rotation = pending.destination.rotation;
    if (!hexGridTileIsValid(tile) || !elevationIsValid(elevation)) {
        tile = host != nullptr ? host->tile : -1;
        elevation = host != nullptr ? host->elevation : -1;
    }
    if (rotation < 0 || rotation >= ROTATION_COUNT) {
        rotation = host != nullptr ? host->rotation : ROTATION_SE;
    }
    if (!placePlayerRosterAtDestination(proposer, tile, elevation, rotation)
        || localPlayerActor() == nullptr
        || map_set_elevation(localPlayerActor()->elevation) != 0
        || !registerWorldObjects()
        || session.transitionTo(SessionPhase::Exploration) != LocalSessionError::None) return;
    // The close event is followed by a complete checkpoint, including the new
    // map, party placement, script locals, quest globals and inventories.
    deferredEvents.push_back(GameEvent { {}, pending.cause,
        SharedModalStateChangedEvent { pending.actorId, SharedModalKind::Dialogue,
            false, SessionPhase::Exploration, session.phaseRevision() } });
}

bool networkWorldEndDialogue()
{
    if (worldMode != NetworkLaunchMode::Host || !activeSharedModal.has_value()
        || activeSharedModal->kind != SharedModalKind::Dialogue
        || session.phase() != SessionPhase::Dialogue
        || dialogueCause.value == 0
        || session.transitionTo(SessionPhase::Exploration) != LocalSessionError::None) {
        return false;
    }
    deferredEvents.push_back(GameEvent { {}, dialogueCause,
        SharedModalStateChangedEvent { activeSharedModal->actorId,
            SharedModalKind::Dialogue, false, SessionPhase::Exploration,
            session.phaseRevision() } });
    activeSharedModal.reset();
    pendingTalk.reset();
    dialoguePresentation.reset();
    dialogueVotes.clear();
    dialogueCause = {};
    return true;
}

bool networkWorldApplyPeerDialogueRequested(const DialogueRequestedEvent& event)
{
    return networkWorldApplyPeerSharedModal(SharedModalStateChangedEvent {
        event.actorId, SharedModalKind::Dialogue, true,
        SessionPhase::Dialogue, event.phaseRevision });
}

bool networkWorldApplyPeerDialogueVote(const DialogueVoteRecordedEvent& event)
{
    PlayerCharacterState* player = session.players().findByActor(event.actorId);
    return player != nullptr
        && dialogueVotes.vote(player->id, event.revision, event.option);
}

bool networkWorldApplyPeerDialoguePresentation(const DialoguePresentationEvent& event)
{
    if (dialoguePresentation.has_value()
        && event.revision == dialoguePresentation->revision) {
        return event.actorId == dialoguePresentation->actorId
            && event.targetId == dialoguePresentation->targetId
            && event.policy == dialoguePresentation->policy
            && event.reply == dialoguePresentation->reply
            && event.options == dialoguePresentation->options;
    }
    if (worldMode != NetworkLaunchMode::Join || !activeSharedModal.has_value()
        || activeSharedModal->kind != SharedModalKind::Dialogue
        || activeSharedModal->actorId != event.actorId
        || session.entities().findObject(event.targetId) == nullptr
        || (dialoguePresentation.has_value()
            && event.revision <= dialoguePresentation->revision)) return false;
    PlayerCharacterState* talker = session.players().findByActor(event.actorId);
    if (talker == nullptr || !dialogueVotes.begin(event.revision, talker->id,
            kHostPlayerId, session.players().playerIds(),
            static_cast<std::uint8_t>(event.options.size()),
            static_cast<DialogueVotingPolicy>(event.policy),
            combatClockMilliseconds() + 60000)) return false;
    dialoguePresentation = event;
    return true;
}

void networkWorldCancelPendingWorldMapProposal()
{
    if (!pendingWorldMapProposal.has_value()) {
        return;
    }
    if (worldMode == NetworkLaunchMode::Host
        && session.phase() == SessionPhase::Exploration
        && pendingWorldMapProposal->proposalSequence.value != 0) {
        deferredEvents.push_back(GameEvent {
            {},
            pendingWorldMapProposal->proposalSequence,
            SharedModalStateChangedEvent {
                pendingWorldMapProposal->proposerActorId,
                SharedModalKind::WorldMap,
                false,
                SessionPhase::Exploration,
                session.phaseRevision(),
            },
        });
    }
    pendingWorldMapProposal.reset();
}

void networkWorldHostTakeOverWorldMapTravel()
{
    if (worldMode != NetworkLaunchMode::Host
        || !session.isActive()
        || session.phase() != SessionPhase::Transition
        || !activeSharedModal.has_value()
        || activeSharedModal->kind != SharedModalKind::WorldMap
        || approvedWorldMapProposerActorId != session.playerActorId(kGuestPlayerId)
        || activeSharedModal->actorId != session.playerActorId(kGuestPlayerId)) {
        return;
    }
    activeSharedModal->actorId = session.playerActorId(kHostPlayerId);
    if (approvedWorldMapProposalSequence.value != 0) {
        deferredEvents.push_back(GameEvent {
            {},
            approvedWorldMapProposalSequence,
            SharedModalStateChangedEvent {
                activeSharedModal->actorId,
                SharedModalKind::WorldMap,
                true,
                SessionPhase::Transition,
                session.phaseRevision(),
            },
        });
    }
}

bool networkWorldSynchronizeEnginePhase()
{
    if (!session.isActive()) {
        return false;
    }
    if (pendingWorldMapProposal.has_value()
        && (pendingWorldMapProposal->expiresAt <= std::chrono::steady_clock::now()
            || pendingWorldMapProposal->map != map_data.field_34
            || pendingWorldMapProposal->phaseRevision != session.phaseRevision()
            || !worldMapProposalSourceReady()
            || isInCombat())) {
        networkWorldCancelPendingWorldMapProposal();
    }
    if (activeSharedModal.has_value()) {
        return session.phase() == sharedModalPhase(activeSharedModal->kind);
    }
    SessionPhase desired = isInCombat() ? SessionPhase::Combat : SessionPhase::Exploration;
    return session.phase() == desired
        || session.transitionTo(desired) == LocalSessionError::None;
}

SessionPhase networkWorldPhase()
{
    return session.phase();
}

std::uint32_t networkWorldPhaseRevision()
{
    return session.phaseRevision();
}

std::optional<EntityId> networkWorldReadyLocalExitGrid()
{
    if (!session.isActive() || session.phase() != SessionPhase::Exploration) {
        return std::nullopt;
    }
    Object* actor = localPlayerActor();
    if (actor == nullptr || anim_busy(actor) == -1) {
        return std::nullopt;
    }
    for (const auto& entry : worldExitGrids) {
        Object* exitGrid = entry.second;
        int destinationMap = -1;
        int destinationTile = -1;
        int destinationElevation = -1;
        int destinationRotation = -1;
        if (exitGrid == nullptr
            || actor->tile != exitGrid->tile
            || actor->elevation != exitGrid->elevation
            || !exitGridDestination(exitGrid,
                destinationMap,
                destinationTile,
                destinationElevation,
                destinationRotation)) {
            continue;
        }
        bool ready = true;
        for (PlayerId playerId : session.players().playerIds()) {
            Object* playerActor = session.entities().findObject(session.playerActorId(playerId));
            if (playerActor == nullptr
                || playerActor->elevation != exitGrid->elevation
                || tile_dist(playerActor->tile, exitGrid->tile) > 4) {
                ready = false;
                break;
            }
        }
        if (ready) {
            return entry.first;
        }
    }
    return std::nullopt;
}

bool networkWorldIsWorldMapExitGrid(EntityId exitId)
{
    Object* exitGrid = session.isActive() ? session.entities().findObject(exitId) : nullptr;
    return isExitGrid(exitGrid) && isWorldMapDestination(exitGrid->data.misc.map);
}

const StoryPresentationState& networkWorldStory() { return storyPresentation; }

bool networkWorldBeginStory(StoryPresentationState state)
{
    if (worldMode != NetworkLaunchMode::Host || storyPresentation.active
        || state.revision <= storyPresentation.revision) return false;
    state.active = true;
    state.completed.clear();
    storyPresentation = std::move(state);
    return true;
}

bool networkWorldCompleteStory(PlayerId player, std::uint64_t revision)
{
    if (worldMode != NetworkLaunchMode::Host || !storyPresentation.active
        || revision != storyPresentation.revision || session.players().find(player) == nullptr) return false;
    if (std::find(storyPresentation.completed.begin(), storyPresentation.completed.end(), player) == storyPresentation.completed.end())
        storyPresentation.completed.push_back(player);
    return true;
}

void networkWorldFinishStory() { if (worldMode == NetworkLaunchMode::Host) storyPresentation.active = false; }

bool networkWorldSharedModalActive()
{
    return storyPresentation.active || activeSharedModal.has_value()
        || session.phase() == SessionPhase::Dialogue
        || session.phase() == SessionPhase::Transition;
}

bool networkWorldLocalWorldMapController()
{
    return session.isActive()
        && session.phase() == SessionPhase::Transition
        && activeSharedModal.has_value()
        && activeSharedModal->kind == SharedModalKind::WorldMap
        && activeSharedModal->actorId == session.playerActorId(
            worldMode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId);
}

Object* networkWorldWorldMapControllerActor()
{
    return networkWorldWorldMapTravelApproved()
        ? session.entities().findObject(activeSharedModal->actorId) : nullptr;
}

std::optional<PlayerId> networkWorldPendingWorldMapProposer()
{
    if (!session.isActive() || !pendingWorldMapProposal.has_value()
        || session.phase() != SessionPhase::Exploration) {
        return std::nullopt;
    }
    const PlayerCharacterState* proposer = session.players().findByActor(
        pendingWorldMapProposal->proposerActorId);
    return proposer != nullptr ? std::optional<PlayerId> { proposer->id } : std::nullopt;
}

bool networkWorldWorldMapTravelApproved()
{
    return session.isActive() && session.phase() == SessionPhase::Transition
        && activeSharedModal.has_value()
        && activeSharedModal->kind == SharedModalKind::WorldMap;
}

bool networkWorldWorldMapDeparted()
{
    return networkWorldWorldMapTravelApproved() && worldMapDeparted;
}

void networkWorldHealRemotePlayersForTravelDay()
{
    if (worldMode == NetworkLaunchMode::Host && session.isActive()) {
        healRemotePlayersForHours(24);
    }
}

std::optional<std::pair<std::int32_t, std::int32_t>> networkWorldSelectedWorldMapRoute()
{
    return selectedWorldMapRoute;
}

bool networkWorldStopWorldMapRoute()
{
    if (worldMode != NetworkLaunchMode::Host || !networkWorldWorldMapTravelApproved()
        || !selectedWorldMapRoute.has_value()) return false;
    WorldMapRouteSelectedEvent stopped { activeSharedModal->actorId, -1, -1, true };
    selectedWorldMapRoute.reset();
    worldmap_authoritative_travel_cancel();
    deferredEvents.push_back(GameEvent { {}, approvedWorldMapProposalSequence, stopped });
    return true;
}

bool networkWorldRunWorldMapRouteStopSmokeTest()
{
    if (worldMode != NetworkLaunchMode::Host) return true;
    if (!networkWorldWorldMapTravelApproved()
        || activeSharedModal->actorId != session.playerActorId(kGuestPlayerId)
        || networkWorldLocalWorldMapController()) return false;
    WorldMapState savedMap;
    worldmap_capture_state(savedMap);
    WorldMapTravelProgress savedProgress;
    worldmap_capture_travel_progress(savedProgress);
    auto savedRoute = selectedWorldMapRoute;
    bool savedDeparted = worldMapDeparted;
    int savedTime = game_time();
    std::size_t savedEventCount = deferredEvents.size();
    std::vector<QueueEventState> savedQueue;
    if (!queue_capture_state(savedQueue)) return false;
    queue_clear();
    int savedWater = game_global_vars[GVAR_VAULT_WATER];
    int savedVats = game_global_vars[GVAR_VATS_COUNTDOWN];
    int savedMaster = game_global_vars[GVAR_COUNTDOWN_TO_DESTRUCTION];
    game_global_vars[GVAR_VAULT_WATER] = 1;
    game_global_vars[GVAR_VATS_COUNTDOWN] = 0;
    game_global_vars[GVAR_COUNTDOWN_TO_DESTRUCTION] = 0;
    bool blocked = false;
    // Exercise the real walkmask rather than manufacturing a Blocked result.
    for (int y = 100; y < 1400 && !blocked; y += 100) {
        for (int x = 100; x < 1000 && !blocked; x += 100) {
            WorldMapState fixture = savedMap;
            fixture.x = x;
            fixture.y = y;
            set_game_time(savedTime);
            if (!worldmap_apply_state(fixture)) continue;
            worldmap_authoritative_travel_cancel();
            selectedWorldMapRoute = std::make_pair(x + 10, y);
            WorldMapTravelStepResult step = worldmap_multiplayer_advance_travel();
            if (step.status != WorldMapTravelStepStatus::Blocked) continue;
            int stoppedTime = game_time();
            blocked = !selectedWorldMapRoute.has_value()
                && worldmap_multiplayer_advance_travel().status == WorldMapTravelStepStatus::Invalid
                && game_time() == stoppedTime;
        }
    }
    WorldMapState terrain = savedMap;
    terrain.x = 1200;
    terrain.y = 1200;
    bool arrived = worldmap_apply_state(terrain);
    worldmap_authoritative_travel_cancel();
    selectedWorldMapRoute = std::make_pair(terrain.x, terrain.y);
    WorldMapTravelStepResult arrival = worldmap_multiplayer_advance_travel();
    arrived = arrived && arrival.status == WorldMapTravelStepStatus::Arrived
        && !selectedWorldMapRoute.has_value();
    const auto* event = deferredEvents.size() > savedEventCount
        ? std::get_if<WorldMapRouteSelectedEvent>(&deferredEvents.back().payload) : nullptr;
    bool published = event != nullptr && event->clear
        && event->actorId == session.playerActorId(kGuestPlayerId);
    while (deferredEvents.size() > savedEventCount) deferredEvents.pop_back();
    selectedWorldMapRoute = savedRoute;
    worldMapDeparted = savedDeparted;
    set_game_time(savedTime);
    game_global_vars[GVAR_VAULT_WATER] = savedWater;
    game_global_vars[GVAR_VATS_COUNTDOWN] = savedVats;
    game_global_vars[GVAR_COUNTDOWN_TO_DESTRUCTION] = savedMaster;
    bool restored = worldmap_apply_state(savedMap)
        && worldmap_apply_travel_progress(savedProgress) && queue_replace_state(savedQueue);
    std::fprintf(stderr, "NATIVE_WORLD_MAP_ROUTE_STOP_%s guest_controller=1 blocked=%d terrain_arrived=%d published=%d restored=%d\n",
        blocked && arrived && published && restored ? "PASS" : "FAIL", blocked, arrived, published, restored);
    return blocked && arrived && published && restored;
}

WorldMapTravelStepResult networkWorldAdvanceWorldMapTravel()
{
    WorldMapTravelStepResult invalid;
    if (worldMode != NetworkLaunchMode::Host
        || !session.isActive()
        || session.phase() != SessionPhase::Transition
        || !activeSharedModal.has_value()
        || activeSharedModal->kind != SharedModalKind::WorldMap
        || !selectedWorldMapRoute.has_value()) {
        return invalid;
    }
    WorldMapTravelProgress progress;
    worldmap_capture_travel_progress(progress);
    WorldMapState position;
    worldmap_capture_state(position);
    if (position.x == selectedWorldMapRoute->first
        && position.y == selectedWorldMapRoute->second) {
        WorldMapTravelStepResult arrived;
        arrived.status = WorldMapTravelStepStatus::Arrived;
        arrived.x = position.x;
        arrived.y = position.y;
        arrived.gameTime = game_time();
        return arrived;
    }
    if (!progress.active
        || progress.targetX != selectedWorldMapRoute->first
        || progress.targetY != selectedWorldMapRoute->second) {
        if (!worldmap_authoritative_travel_begin(selectedWorldMapRoute->first,
                selectedWorldMapRoute->second)) {
            return invalid;
        }
    }
    WorldMapTravelStepResult result = worldmap_authoritative_travel_step();
    if (result.x != worldMapOriginX || result.y != worldMapOriginY) {
        worldMapDeparted = true;
    }
    if (result.dayElapsed) {
        healRemotePlayersForHours(24);
    }
    return result;
}

bool networkWorldFinishWorldMapTravel(WorldMapArrivalKind kind, int specialEncounter, int forcedMap)
{
    CommandSequence causedBy = approvedWorldMapProposalSequence;
    std::optional<WorldMapArrivedEvent> arrival = completeWorldMapTravel(kind, specialEncounter, forcedMap);
    if (!arrival.has_value()) return false;
    deferredEvents.push_back(GameEvent { {}, causedBy, std::move(*arrival) });
    publishSharedActivity(SharedActivityKind::WorldOutcome, forcedMap,
        static_cast<std::int32_t>(kind), "World-map travel ended");
    return true;
}

const SnapshotCaptureDiagnostic& networkWorldLastSnapshotCaptureDiagnostic()
{
    return lastCaptureDiagnostic;
}

bool networkWorldRunStoppedRestSmoke()
{
    Object* host = networkWorldPlayerActor(kHostPlayerId);
    Object* guest = networkWorldPlayerActor(kGuestPlayerId);
    if (!networkRuntimeSimulationStopped() || host == nullptr || guest == nullptr) return false;
    int time = game_time(), hostHp = critter_get_hits(host), guestHp = critter_get_hits(guest);
    return advanceSharedRest(180, false) && game_time() == time
        && critter_get_hits(host) == hostHp && critter_get_hits(guest) == guestHp;
}

bool networkWorldRunVariableCapacitySmoke(bool (*capture)(EventSequence, WorldSnapshot&))
{
    if (worldMode != NetworkLaunchMode::Host || capture == nullptr) return false;
    struct Restore {
        int count = num_game_global_vars;
        ~Restore() { num_game_global_vars = count; }
    } restore;
    // Reject the count before reading native storage. This exercises the real
    // capture path without allocating thousands of campaign objects.
    num_game_global_vars = static_cast<int>(kMaxSnapshotVariables) + 1;
    WorldSnapshot snapshot;
    snapshot.mapId = -99;
    bool rejected = !capture({}, snapshot);
    return rejected && snapshot.mapId == -99
        && lastCaptureDiagnostic.failure == SnapshotCaptureFailure::SnapshotValidation
        && lastCaptureDiagnostic.snapshotError == SnapshotError::TooManyVariables;
}

bool networkWorldCaptureSnapshot(EventSequence lastIncludedEvent, WorldSnapshot& snapshot)
{
    lastCaptureDiagnostic = {};
    if (!session.isActive()) {
        return captureFailure(SnapshotCaptureFailure::InactiveSession, "session");
    }
    if (session.phase() != SessionPhase::Exploration) {
        for (const auto& entry : theftAccess) activeLootTargets.erase(entry.first);
        theftAccess.clear();
    }

    // Native dialogue scripts can create rewards directly in an inventory.
    // Register them before capturing so the checkpoint carries the complete
    // inventory, including rewards created outside command execution.
    if (worldMode == NetworkLaunchMode::Host) {
        // Map registration resets active interactions. Discover native script
        // spawns incrementally so a checkpoint preserves combat and dialogue.
        if (!discoverUntrackedWorldObjects()) return false;
        for (PlayerId playerId : session.players().playerIds()) {
            if (!registerUntrackedInventory(networkWorldPlayerActor(playerId))) return captureFailure(SnapshotCaptureFailure::WorldRegistration, "player_inventory");
        }
        for (const auto& entry : worldCritters) {
            if (!registerUntrackedInventory(entry.second)) return captureFailure(SnapshotCaptureFailure::WorldRegistration, "npc_inventory", entry.first);
        }
        for (const auto& entry : worldDoors) {
            if (!registerUntrackedInventory(entry.second)) return captureFailure(SnapshotCaptureFailure::WorldRegistration, "door_inventory", entry.first);
        }
        for (const auto& entry : worldScenery) {
            if (!registerUntrackedInventory(entry.second)) return captureFailure(SnapshotCaptureFailure::WorldRegistration, "scenery_inventory", entry.first);
        }
        // Registration can append worldItems; index the original owners so
        // recursion does not invalidate iterators over that vector.
        std::size_t ownerCount = worldItems.size();
        for (std::size_t index = 0; index < ownerCount; ++index) {
            Object* owner = worldItems[index].second;
            if (!registerUntrackedInventory(owner)) return captureFailure(SnapshotCaptureFailure::WorldRegistration, "item_inventory", worldItems[index].first);
        }
    }

    WorldSnapshot captured;
    captured.lastIncludedEvent = lastIncludedEvent;
    captured.phase = session.phase();
    captured.phaseRevision = session.phaseRevision();
    captured.mapId = map_data.field_34;
    if (captured.phase == SessionPhase::Combat) {
        captured.combat = combatTurns.snapshot(combatClockMilliseconds());
        captured.combatFreeMove = combat_free_move;
    }
    captured.gameTime = game_time();
    captured.story = storyPresentation;
    captured.sharedActivity.assign(sharedActivity.begin(), sharedActivity.end());
    if (directTradeController.active()) {
        captured.directTrade = directTradeController.state();
    }
    if (captured.phase == SessionPhase::Dialogue
        && activeSharedModal.has_value()
        && activeSharedModal->kind == SharedModalKind::Dialogue) {
        captured.dialogueActorId = activeSharedModal->actorId;
        captured.dialoguePresentation = dialoguePresentation;
        captured.dialogueBallots = dialogueVotes.ballots();
    }
    worldmap_capture_state(captured.worldMap);
    worldmap_capture_travel_progress(captured.worldMapTravel.progress);
    if (pendingWorldMapProposal.has_value()) {
        captured.worldMapTravel.proposerActorId = pendingWorldMapProposal->proposerActorId;
        captured.worldMapTravel.controllerActorId = pendingWorldMapProposal->proposerActorId;
        captured.worldMapTravel.stage = WorldMapTravelStage::Proposed;
    } else if (activeSharedModal.has_value()
        && activeSharedModal->kind == SharedModalKind::WorldMap) {
        captured.worldMapTravel.proposerActorId = approvedWorldMapProposerActorId;
        captured.worldMapTravel.controllerActorId = activeSharedModal->actorId;
        captured.worldMapTravel.stage = WorldMapTravelStage::Approved;
        if (selectedWorldMapRoute.has_value()) {
            captured.worldMapTravel.targetX = selectedWorldMapRoute->first;
            captured.worldMapTravel.targetY = selectedWorldMapRoute->second;
        }
    }
    for (PlayerId playerId : session.players().playerIds()) {
        EntityId actorId = session.playerActorId(playerId);
        Object* actor = session.entities().findObject(actorId);
        PlayerCharacterState* player = session.players().find(playerId);
        if (actor == nullptr || player == nullptr || anim_busy(actor) == -1) {
            return captureFailure(actor != nullptr && player != nullptr
                    ? SnapshotCaptureFailure::BusyObject : SnapshotCaptureFailure::MissingObject,
                "actors", actorId);
        }
        captured.actors.push_back(ActorSnapshot {
            actorId,
            playerId,
            actor->tile,
            actor->elevation,
            actor->rotation,
            std::max(critter_get_hits(actor), 0),
            std::max(actor->data.critter.combat.ap, 0),
            actor->data.critter.combat.results,
            actor->fid,
            actor->frame,
            sharedObjectFlags(actor),
            actor->lightDistance,
            actor->lightIntensity,
            actor->data.critter.combat.maneuver,
            actor->data.critter.combat.damageLastTurn,
            actor->data.critter.combat.team,
            session.entities().findEntity(actor->data.critter.combat.whoHitMe)
                .value_or(EntityId {}),
            player->build,
            std::max(actor->data.critter.poison, 0),
            std::max(actor->data.critter.radiation, 0),
        });
    }
    for (const auto& entry : worldScenery) {
        Object* scenery = entry.second;
        if (scenery == nullptr
            || session.entities().findObject(entry.first) != scenery
            || anim_busy(scenery) == -1
            || scenery->tile < 0) {
            return captureFailure(scenery != nullptr && session.entities().findObject(entry.first) == scenery
                    && anim_busy(scenery) == -1 ? SnapshotCaptureFailure::BusyObject : SnapshotCaptureFailure::MissingObject,
                "scenery", entry.first);
        }
        captured.scenery.push_back(ScenerySnapshot {
            entry.first,
            scenery->pid,
            scenery->fid,
            scenery->tile,
            scenery->elevation,
            scenery->rotation,
            scenery->frame,
            sharedObjectFlags(scenery),
            scenery->lightDistance,
            scenery->lightIntensity,
            scenery->data.scenery.stairs.destinationMap,
            scenery->data.scenery.stairs.destinationBuiltTile,
        });
    }
    for (const auto& entry : worldCritters) {
        Object* critter = entry.second;
        if (critter == nullptr
            || session.entities().findObject(entry.first) != critter
            || critter->tile < 0) {
            return captureFailure(SnapshotCaptureFailure::MissingObject, "critters", entry.first);
        }
        captured.critters.push_back(CritterSnapshot {
            entry.first,
            critter->pid,
            critter->tile,
            critter->elevation,
            critter->rotation,
            std::max(critter_get_hits(critter), 0),
            std::max(critter->data.critter.combat.ap, 0),
            critter->data.critter.combat.results,
            critter->data.critter.combat.team,
            critter->fid,
            critter->frame,
            sharedObjectFlags(critter),
            critter->lightDistance,
            critter->lightIntensity,
            critter->data.critter.combat.maneuver,
            critter->data.critter.combat.damageLastTurn,
            session.entities().findEntity(critter->data.critter.combat.whoHitMe)
                .value_or(EntityId {}),
            isPartyMember(critter),
        });
    }
    for (const auto& entry : worldDoors) {
        Object* door = entry.second;
        if (door == nullptr
            || session.entities().findObject(entry.first) != door
            || anim_busy(door) == -1) {
            return captureFailure(door != nullptr && session.entities().findObject(entry.first) == door
                    && anim_busy(door) == -1 ? SnapshotCaptureFailure::BusyObject : SnapshotCaptureFailure::MissingObject,
                "doors", entry.first);
        }
        captured.doors.push_back(DoorSnapshot {
            entry.first,
            obj_is_open(door) != 0,
            obj_is_locked(door),
            door->frame,
        });
    }
    for (const auto& entry : worldItems) {
        Object* item = entry.second;
        if (item == nullptr || session.entities().findObject(entry.first) != item) {
            return captureFailure(SnapshotCaptureFailure::MissingObject, "items", entry.first);
        }
        ItemSnapshot itemState;
        itemState.entityId = entry.first;
        if (!describeItem(item, itemState.itemDescriptor)) {
            return captureFailure(SnapshotCaptureFailure::ItemDescriptor, "items", entry.first);
        }
        if (item->owner == nullptr) {
            if (item->tile < 0 || !elevationIsValid(item->elevation)) {
                return captureFailure(SnapshotCaptureFailure::InvalidItemOwner, "ground_item", entry.first);
            }
            itemState.tile = item->tile;
            itemState.elevation = item->elevation;
            itemState.quantity = 1;
        } else {
            std::optional<EntityId> holderId = session.entities().findEntity(item->owner);
            int quantity = item_count(item->owner, item);
            if (!holderId.has_value() || quantity <= 0) {
                return captureFailure(SnapshotCaptureFailure::InvalidItemOwner, "inventory_item", entry.first);
            }
            itemState.holderId = *holderId;
            itemState.quantity = static_cast<std::uint32_t>(quantity);
        }
        itemState.fid = item->fid;
        itemState.frame = item->frame;
        itemState.objectFlags = sharedObjectFlags(item);
        itemState.lightDistance = item->lightDistance;
        itemState.lightIntensity = item->lightIntensity;
        captured.items.push_back(itemState);
    }
    if (num_game_global_vars > static_cast<int>(kMaxSnapshotVariables)
        || num_map_global_vars > static_cast<int>(kMaxSnapshotVariables)
        || num_map_local_vars > static_cast<int>(kMaxSnapshotVariables)) {
        return captureFailure(SnapshotCaptureFailure::SnapshotValidation, "variables", {}, SnapshotError::TooManyVariables);
    }
    if (!captureVariables(game_global_vars, num_game_global_vars, captured.gameGlobalVariables)
        || !captureVariables(map_global_vars, num_map_global_vars, captured.mapGlobalVariables)
        || !captureVariables(map_local_vars, num_map_local_vars, captured.mapLocalVariables)) {
        return captureFailure(SnapshotCaptureFailure::VariableState, "variables");
    }
    if (!captureTimedEvents(captured)) return false;
    SnapshotError error = validateSnapshot(captured);
    if (error != SnapshotError::None) {
        if (lastReportedCaptureDiagnostic.failure != SnapshotCaptureFailure::SnapshotValidation
            || lastReportedCaptureDiagnostic.snapshotError != error
            || lastReportedCaptureDiagnostic.mapId != map_data.field_34) {
            std::fprintf(stderr, "MULTIPLAYER_SNAPSHOT_VALIDATION_FAILED error=%d globals=%zu map_globals=%zu locals=%zu timers=%zu.\n",
                static_cast<int>(error), captured.gameGlobalVariables.size(), captured.mapGlobalVariables.size(),
                captured.mapLocalVariables.size(), captured.timedEvents.size());
            if (error == SnapshotError::InvalidSceneryState) {
                for (const auto& state : captured.scenery) {
                    if (state.pid < 0 || PID_TYPE(state.pid) != OBJ_TYPE_SCENERY
                        || state.fid < 0 || FID_TYPE(state.fid) != OBJ_TYPE_SCENERY
                        || !hexGridTileIsValid(state.tile) || !elevationIsValid(state.elevation)
                        || state.rotation < 0 || state.rotation >= ROTATION_COUNT || state.frame < 0
                        || state.lightDistance < 0 || state.lightDistance > 8
                        || state.lightIntensity < 0 || state.lightIntensity > 65536) {
                        std::fprintf(stderr, "MULTIPLAYER_SNAPSHOT_SCENERY_INVALID entity=%u pid=%d fid=%d tile=%d elevation=%d rotation=%d frame=%d light=%d/%d.\n",
                            state.entityId.value, state.pid, state.fid, state.tile, state.elevation,
                            state.rotation, state.frame, state.lightDistance, state.lightIntensity);
                    }
                }
            }
        }
        return captureFailure(SnapshotCaptureFailure::SnapshotValidation, "validation", {}, error);
    }
    lastReportedCaptureDiagnostic = {};
    snapshot = std::move(captured);
    return true;
}

bool networkWorldApplySnapshot(const WorldSnapshot& snapshot, bool preserveMovement)
{
    SnapshotError snapshotError = validateSnapshot(snapshot);
    if (session.isActive() && snapshotError == SnapshotError::None
        && !validateItemTimerOwnerFlags(snapshot)) {
        std::fprintf(stderr, "Multiplayer snapshot item timer owner lacks native USED flag.\n");
        return false;
    }
    // Identity and phase checks must precede map replacement or object
    // reconciliation. A rejected checkpoint must not delete native items.
    if (session.isActive() && snapshotError == SnapshotError::None
        && (!validateActorState(snapshot)
            || snapshot.phaseRevision < session.phaseRevision()
            || (snapshot.phaseRevision == session.phaseRevision()
                && snapshot.phase != session.phase()))) {
        std::fprintf(stderr,
            "Multiplayer snapshot rejected before native mutation: actors=%d phase=%d/%d revision=%u/%u.\n",
            validateActorState(snapshot) ? 1 : 0,
            static_cast<int>(snapshot.phase), static_cast<int>(session.phase()),
            snapshot.phaseRevision, session.phaseRevision());
        return false;
    }
    // Game globals belong to the loaded game rather than a particular map.
    // Reject their incompatible shape before replacing the native map.
    if (session.isActive() && snapshotError == SnapshotError::None
        && !validateGameGlobalVariables(snapshot)) {
        std::fprintf(stderr,
            "Multiplayer snapshot game globals rejected before native mutation: count=%zu/%d.\n",
            snapshot.gameGlobalVariables.size(), num_game_global_vars);
        return false;
    }
    // Reject unavailable prototypes before map replacement. Missing native
    // bodies and their exact placement are staged below before reconciliation.
    if (session.isActive() && snapshotError == SnapshotError::None
        && !validateSnapshotPrototypes(snapshot)) {
        return false;
    }
    if (session.isActive() && snapshotError == SnapshotError::None
        && !validateSnapshotActorArt(snapshot)) {
        return false;
    }
    if (session.isActive()
        && snapshotError == SnapshotError::None
        && networkWorldReplicaSessionActive()
        && snapshot.mapId != map_data.field_34
        && (!loadSharedMap(snapshot.mapId, true) || !registerWorldObjects())) {
        std::fprintf(stderr,
            "Multiplayer snapshot could not load authoritative map %d.\n",
            snapshot.mapId);
        return false;
    }
    // Map globals and static doors must match the authoritative target map.
    // Check them before resizing locals, which may free or expand native data.
    // Loading a different map above remains a separate rollback boundary.
    if (session.isActive() && snapshotError == SnapshotError::None
        && (!validateMapGlobalVariables(snapshot)
            || snapshot.doors.size() != worldDoors.size()
            || (!networkWorldReplicaSessionActive() && snapshot.scenery.size() != worldScenery.size()))) {
        std::fprintf(stderr,
            "Multiplayer snapshot map layout rejected before local resize: map_globals=%zu/%d doors=%zu/%zu scenery=%zu/%zu.\n",
            snapshot.mapGlobalVariables.size(), num_map_global_vars,
            snapshot.doors.size(), worldDoors.size(), snapshot.scenery.size(), worldScenery.size());
        return false;
    }
    SnapshotReconciliationPlan reconciliation;
    if (session.isActive() && snapshotError == SnapshotError::None) {
        selectSnapshotBodies(snapshot, reconciliation);
    }
    if (session.isActive() && snapshotError == SnapshotError::None
        && !validateSnapshotCritterFrames(snapshot, reconciliation)) {
        return false;
    }
    if (session.isActive() && snapshotError == SnapshotError::None
        && !validateSnapshotObjectFrames(snapshot, reconciliation)) {
        return false;
    }
    if (session.isActive() && snapshotError == SnapshotError::None
        && !stageSnapshotBodies(snapshot, reconciliation)) {
        std::fprintf(stderr, "Multiplayer snapshot native creation or placement failed before reconciliation.\n");
        return false;
    }
    PreparedQueueEvents preparedTimers;
    std::vector<Object*> preparedTimerOwners;
    if (session.isActive() && snapshotError == SnapshotError::None
        && !prepareTimedEvents(snapshot, reconciliation, preparedTimerOwners, preparedTimers)) {
        std::fprintf(stderr, "Multiplayer snapshot timed-event preparation failed before reconciliation.\n");
        return false;
    }
    if (session.isActive()
        && snapshotError == SnapshotError::None
        && networkWorldReplicaSessionActive()
        && !map_resize_local_vars(static_cast<int>(snapshot.mapLocalVariables.size()))) {
        return false;
    }
    bool variablesValid = validateVariableState(snapshot);
    if (!session.isActive()
        || snapshotError != SnapshotError::None
        || snapshot.doors.size() != worldDoors.size()
        || (!networkWorldReplicaSessionActive() && snapshot.scenery.size() != worldScenery.size())
        || !variablesValid) {
        std::fprintf(stderr,
            "Multiplayer snapshot preflight failed: active=%d error=%d doors=%zu/%zu scenery=%zu/%zu variables=%d globals=%zu/%d map_globals=%zu/%d map_locals=%zu/%d.\n",
            session.isActive() ? 1 : 0,
            static_cast<int>(snapshotError),
            snapshot.doors.size(),
            worldDoors.size(),
            snapshot.scenery.size(),
            worldScenery.size(),
            variablesValid ? 1 : 0,
            snapshot.gameGlobalVariables.size(), num_game_global_vars,
            snapshot.mapGlobalVariables.size(), num_map_global_vars,
            snapshot.mapLocalVariables.size(), num_map_local_vars);
        return false;
    }

    if (snapshot.phase != SessionPhase::Dialogue || snapshot.phaseRevision != session.phaseRevision()) {
        npcBarterState.reset();
        pendingScriptedNpcBarter.reset();
    }
    reconciliation.commit();
    bool sceneryLayoutMismatch = reconciliation.reconstructScenery;
    if (snapshot.phase != SessionPhase::Exploration) {
        for (const auto& entry : theftAccess) activeLootTargets.erase(entry.first);
        theftAccess.clear();
    }
    if (networkWorldReplicaSessionActive() && sceneryLayoutMismatch) {
        // Saved terrain and map-enter scripts can have a different scenery
        // population from the installed MAP. Replicas reconstruct descriptors
        // instead of running the authority's spawning/removal scripts.
        auto localScenery = worldScenery;
        std::vector<std::pair<EntityId, Object*>> reconstructed;
        std::unordered_set<Object*> retained;
        reconstructed.reserve(snapshot.scenery.size());
        for (const ScenerySnapshot& state : snapshot.scenery) {
            Object* matched = reconciliation.find(state.entityId);
            if (!session.entities().findEntity(matched).has_value()
                && !session.registerWorldObject(matched)) return false;
            auto entityId = session.entities().findEntity(matched);
            if (!entityId.has_value()) return false;
            retained.insert(matched);
            reconstructed.emplace_back(*entityId, matched);
        }
        for (const auto& entry : localScenery) {
            if (retained.count(entry.second) == 0) {
                session.entities().unregisterEntity(entry.first);
                obj_erase_object(entry.second, nullptr);
            }
        }
        worldScenery = std::move(reconstructed);
    }

    bool staticRegistryMismatch = false;
    for (std::size_t index = 0; index < snapshot.doors.size(); index++) {
        if (session.entities().findObject(snapshot.doors[index].entityId)
            != worldDoors[index].second) {
            staticRegistryMismatch = true;
            break;
        }
    }
    for (std::size_t index = 0;
         !staticRegistryMismatch && index < snapshot.scenery.size(); index++) {
        if (session.entities().findObject(snapshot.scenery[index].entityId)
            != worldScenery[index].second) {
            staticRegistryMismatch = true;
        }
    }
    if (staticRegistryMismatch) {
        auto updateTrackedId = [](Object* object, EntityId entityId) {
            auto update = [object, entityId](auto& entries) {
                for (auto& entry : entries) {
                    if (entry.second == object) entry.first = entityId;
                }
            };
            update(worldExitGrids);
            update(worldDoors);
            update(worldScenery);
            update(worldCritters);
            update(worldItems);
        };
        auto assignAuthoritativeId = [&](Object* object, EntityId entityId) {
            std::optional<EntityId> current = session.entities().findEntity(object);
            if (!current.has_value()) return false;
            if (*current == entityId) return true;
            Object* displaced = session.entities().findObject(entityId);
            if (session.entities().unregisterEntity(*current)
                    != EntityRegistryError::None
                || (displaced != nullptr
                    && session.entities().unregisterEntity(entityId)
                        != EntityRegistryError::None)
                || session.entities().restoreObject(entityId, object)
                    != EntityRegistryError::None) {
                return false;
            }
            updateTrackedId(object, entityId);
            if (displaced != nullptr) {
                if (session.entities().restoreObject(*current, displaced)
                        != EntityRegistryError::None) {
                    return false;
                }
                updateTrackedId(displaced, *current);
            }
            return true;
        };

        for (std::size_t index = 0; index < worldDoors.size(); index++) {
            if (!assignAuthoritativeId(worldDoors[index].second,
                    snapshot.doors[index].entityId)) {
                return false;
            }
        }
        for (std::size_t index = 0; index < worldScenery.size(); index++) {
            Object* scenery = worldScenery[index].second;
            const ScenerySnapshot& state = snapshot.scenery[index];
            if (scenery == nullptr
                || scenery->pid != state.pid
                || !assignAuthoritativeId(scenery, state.entityId)) {
                std::fprintf(stderr,
                    "Multiplayer snapshot could not rebase scenery entity %u.\n",
                    state.entityId.value);
                return false;
            }
        }
    }

    if (!validateActorState(snapshot)) {
        std::fprintf(stderr, "Multiplayer snapshot actor identity preflight failed.\n");
        return false;
    }
    for (const DoorSnapshot& doorState : snapshot.doors) {
        Object* door = session.entities().findObject(doorState.entityId);
        if (door == nullptr
            || !obj_is_a_portal(door)
            || doorState.open != (doorState.frame != 0)) {
            std::fprintf(stderr,
                "Multiplayer snapshot door preflight failed: entity=%u found=%d portal=%d open=%d frame=%d.\n",
                doorState.entityId.value, door != nullptr ? 1 : 0,
                door != nullptr && obj_is_a_portal(door) ? 1 : 0,
                doorState.open ? 1 : 0, doorState.frame);
            return false;
        }
    }
    for (const ScenerySnapshot& sceneryState : snapshot.scenery) {
        Object* scenery = session.entities().findObject(sceneryState.entityId);
        if (scenery == nullptr
            || FID_TYPE(scenery->fid) != OBJ_TYPE_SCENERY
            || scenery->pid != sceneryState.pid) {
            std::fprintf(stderr,
                "Multiplayer snapshot scenery preflight failed: entity=%u found=%d type=%d/%d pid=%d/%d.\n",
                sceneryState.entityId.value, scenery != nullptr ? 1 : 0,
                scenery != nullptr ? FID_TYPE(scenery->fid) : -1,
                OBJ_TYPE_SCENERY, scenery != nullptr ? scenery->pid : -1,
                sceneryState.pid);
            return false;
        }
    }
    // A map-load script can create or remove an item only on the authority,
    // shifting the sequential IDs assigned to otherwise identical static map
    // items. Rebind every local item against the authoritative holder/location
    // and descriptor before applying state instead of trusting that local load
    // order. Doors and scenery have already passed identity validation above.
    std::vector<Object*> localItems;
    localItems.reserve(worldItems.size());
    for (const auto& entry : worldItems) {
        if (entry.second != nullptr) {
            localItems.push_back(entry.second);
        }
        session.entities().unregisterEntity(entry.first);
    }
    worldItems.clear();
    // Encounter and map-enter scripts can create or remove critters only on
    // the authority. Reconcile them before inventory holders and timed-event
    // owners are resolved; replicas never execute the spawning scripts.
    if (reconciliation.reconstructCritters) {
        std::vector<Object*> localCritters;
        localCritters.reserve(worldCritters.size());
        for (const auto& entry : worldCritters) {
            localCritters.push_back(entry.second);
            session.entities().unregisterEntity(entry.first);
        }
        worldCritters.clear();
        std::unordered_set<Object*> reboundCritters;
        for (const CritterSnapshot& critterState : snapshot.critters) {
            Object* matched = reconciliation.find(critterState.entityId);
            if (session.entities().restoreObject(critterState.entityId, matched) != EntityRegistryError::None) {
                std::fprintf(stderr,
                    "Multiplayer snapshot could not rebind critter pid=%d tile=%d elevation=%d.\n",
                    critterState.pid, critterState.tile, critterState.elevation);
                return false;
            }
            reboundCritters.insert(matched);
            worldCritters.emplace_back(critterState.entityId, matched);
        }
        for (Object* stale : localCritters) {
            if (reboundCritters.find(stale) == reboundCritters.end()) {
                // localItems still contains this critter's inventory pointers.
                // Preserve those objects until the item reconciliation below;
                // erasing an owner normally frees its entire inventory tree.
                while (stale->data.inventory.length > 0) {
                    InventoryItem entry = stale->data.inventory.items[0];
                    if (item_remove_mult(stale, entry.item, entry.quantity) != 0) return false;
                }
                for (auto it = activeLootTargets.begin(); it != activeLootTargets.end();) {
                    if (it->second == stale) it = activeLootTargets.erase(it);
                    else ++it;
                }
                if (isPartyMember(stale) && partyMemberRemove(stale) != 0) return false;
                obj_erase_object(stale, nullptr);
            }
        }
        if (!validateCritterState(snapshot)) {
            std::fprintf(stderr, "Multiplayer snapshot critter identity preflight failed.\n");
            return false;
        }
    }
    // Item IDs describe identity, not nesting order. An older item can move
    // into a newly created container with a higher ID. Restore holders first;
    // the snapshot validator has already rejected dangling owners and cycles.
    const auto& orderedItems = reconciliation.orderedItems;
    std::unordered_set<Object*> reboundItems;
    for (const ItemSnapshot* orderedItem : orderedItems) {
        const ItemSnapshot& itemState = *orderedItem;
        Object* desiredHolder = isValid(itemState.holderId)
            ? session.entities().findObject(itemState.holderId)
            : nullptr;
        if (isValid(itemState.holderId) && desiredHolder == nullptr) {
            std::fprintf(stderr, "Multiplayer snapshot item holder missing: item=%u holder=%u.\n",
                itemState.entityId.value, itemState.holderId.value);
            return false;
        }

        Object* matched = reconciliation.find(itemState.entityId);
        if (matched == nullptr
            || session.entities().restoreObject(itemState.entityId, matched) != EntityRegistryError::None) {
            std::fprintf(stderr, "Multiplayer snapshot item rebind failed: item=%u pid=%d.\n",
                itemState.entityId.value, itemState.itemDescriptor.pid);
            return false;
        }
        reboundItems.insert(matched);
        trackWorldItem(itemState.entityId, matched);
    }
    std::unordered_set<Object*> removedItemPointers;
    for (Object* item : localItems) {
        if (reboundItems.find(item) == reboundItems.end()) {
            removedItemPointers.insert(item);
        }
    }
    // A retained child must survive if an obsolete container is removed.
    for (Object* item : reboundItems) {
        if (item->owner != nullptr && removedItemPointers.count(item->owner) != 0) {
            Object* owner = item->owner;
            int quantity = item_count(owner, item);
            if (quantity <= 0 || item_remove_mult(owner, item, quantity) != 0) return false;
        }
    }
    // Determine roots before deleting anything. Destroying a container also
    // frees its children, so iterating their pointers afterward is unsafe.
    std::vector<Object*> removedRoots;
    for (Object* item : removedItemPointers) {
        if (item != nullptr && removedItemPointers.count(item->owner) == 0) {
            removedRoots.push_back(item);
        }
    }
    for (Object* item : removedRoots) obj_destroy(item);

    if (session.applyAuthoritativePhase(snapshot.phase, snapshot.phaseRevision) != LocalSessionError::None
        || !applyActorAndCritterState(snapshot, preserveMovement)) {
        std::fprintf(stderr, "Multiplayer snapshot failed phase or actor application.\n");
        return false;
    }
    storyPresentation = snapshot.story;
    if (snapshot.phase == SessionPhase::Combat && !snapshot.combat.initiative.empty()) {
        if (!combatTurns.restore(snapshot.combat, combatClockMilliseconds())) {
            return false;
        }
    } else {
        combatTurns.stop();
        openInventories.clear();
        openLootTurns.clear();
    }
    combat_free_move = snapshot.combatFreeMove;
    sharedActivity.assign(snapshot.sharedActivity.begin(), snapshot.sharedActivity.end());
    nextSharedActivityId = sharedActivity.empty()
        ? 1 : sharedActivity.back().id + 1;
    if (snapshot.phase == SessionPhase::Dialogue
        && snapshot.directTrade.has_value()) {
        if (directTradeController.restore(*snapshot.directTrade)
                != DirectTradeResult::Accepted) {
            return false;
        }
        activeSharedModal = ActiveSharedModal {
            snapshot.directTrade->participants[0].actorId,
            SharedModalKind::Barter,
        };
        dialoguePresentation.reset();
        dialogueVotes.clear();
    } else if (snapshot.phase == SessionPhase::Dialogue) {
        directTradeController.clear();
        activeSharedModal = ActiveSharedModal {
            snapshot.dialogueActorId, SharedModalKind::Dialogue };
        dialoguePresentation = snapshot.dialoguePresentation;
        dialogueVotes.clear();
        if (snapshot.dialoguePresentation.has_value()) {
            std::vector<PlayerId> eligible;
            eligible.reserve(snapshot.dialogueBallots.size());
            for (const DialogueBallot& ballot : snapshot.dialogueBallots) {
                eligible.push_back(ballot.playerId);
            }
            PlayerCharacterState* talker = session.players().findByActor(
                snapshot.dialogueActorId);
            if (talker == nullptr || !dialogueVotes.begin(
                    snapshot.dialoguePresentation->revision, talker->id,
                    kHostPlayerId, eligible,
                    static_cast<std::uint8_t>(snapshot.dialoguePresentation->options.size()),
                    static_cast<DialogueVotingPolicy>(snapshot.dialoguePresentation->policy),
                    combatClockMilliseconds() + 60000)) return false;
            for (const DialogueBallot& ballot : snapshot.dialogueBallots) {
                if (ballot.option.has_value()) {
                    dialogueVotes.vote(ballot.playerId,
                        snapshot.dialoguePresentation->revision, *ballot.option);
                }
                if (!ballot.connected) {
                    dialogueVotes.setConnected(ballot.playerId, false);
                }
            }
        }
    } else {
        directTradeController.clear();
        npcBarterState.reset();
        pendingScriptedNpcBarter.reset();
        activeSharedModal.reset();
        dialoguePresentation.reset();
        dialogueVotes.clear();
    }
    Object* localActor = localPlayerActor();
    if (localActor == nullptr
        || (map_elevation != localActor->elevation && map_set_elevation(localActor->elevation) != 0)) {
        std::fprintf(stderr, "Multiplayer snapshot failed local elevation application.\n");
        return false;
    }
    for (const DoorSnapshot& doorState : snapshot.doors) {
        Object* door = session.entities().findObject(doorState.entityId);
        Rect dirtyRect;
        if (obj_set_frame(door, doorState.frame, &dirtyRect) == -1
            || (doorState.locked ? obj_lock(door) : obj_unlock(door)) == -1) {
            std::fprintf(stderr, "Multiplayer snapshot failed door application.\n");
            return false;
        }
        tile_refresh_rect(&dirtyRect, door->elevation);
    }
    for (const ScenerySnapshot& sceneryState : snapshot.scenery) {
        Object* scenery = session.entities().findObject(sceneryState.entityId);
        Rect dirtyRect {};
        if ((scenery->tile != sceneryState.tile || scenery->elevation != sceneryState.elevation)
            && obj_move_to_tile(scenery, sceneryState.tile, sceneryState.elevation, &dirtyRect) == -1) {
            return false;
        }
        if (scenery->rotation != sceneryState.rotation
            && obj_set_rotation(scenery, sceneryState.rotation, &dirtyRect) == -1) {
            return false;
        }
        if (!applySharedObjectPresentation(scenery,
                sceneryState.fid,
                sceneryState.frame,
                sceneryState.objectFlags,
                sceneryState.lightDistance,
                sceneryState.lightIntensity)) {
            std::fprintf(stderr, "Multiplayer snapshot failed scenery presentation application.\n");
            return false;
        }
        scenery->data.scenery.stairs.destinationMap = sceneryState.data0;
        scenery->data.scenery.stairs.destinationBuiltTile = sceneryState.data1;
        tile_refresh_rect(&dirtyRect, scenery->elevation);
    }
    for (const ItemSnapshot& itemState : snapshot.items) {
        Object* item = session.entities().findObject(itemState.entityId);
        if (!applySharedObjectPresentation(item,
                itemState.fid,
                itemState.frame,
                itemState.objectFlags,
                itemState.lightDistance,
                itemState.lightIntensity)) {
            std::fprintf(stderr, "Multiplayer snapshot failed item presentation application.\n");
            return false;
        }
    }
    for (const ItemSnapshot* orderedItem : orderedItems) {
        const ItemSnapshot& itemState = *orderedItem;
        Object* item = session.entities().findObject(itemState.entityId);
        if (!applyItemDescriptor(item, itemState.itemDescriptor)) {
            std::fprintf(stderr,
                "Multiplayer snapshot failed item descriptor application: entity=%u expected_pid=%d actual_pid=%d actual_fid=%d.\n",
                itemState.entityId.value,
                itemState.itemDescriptor.pid,
                item != nullptr ? item->pid : -1,
                item != nullptr ? item->fid : -1);
            return false;
        }
        Object* desiredHolder = isValid(itemState.holderId)
            ? session.entities().findObject(itemState.holderId)
            : nullptr;
        if (desiredHolder != nullptr) {
            if (item->owner == desiredHolder) {
                if (!setInventoryQuantity(desiredHolder, item, itemState.quantity)) {
                    return false;
                }
                continue;
            } else if (item->owner != nullptr) {
                if (!applyInventoryTransfer(item->owner, desiredHolder, item,
                        static_cast<std::uint32_t>(item_count(item->owner, item)), true)) {
                    return false;
                }
            } else {
                inventoryTransferInProgress = true;
                int rc = item_add_force(desiredHolder, item, 1);
                if (rc == 0) {
                    rc = obj_disconnect(item, nullptr);
                }
                inventoryTransferInProgress = false;
                if (rc != 0) {
                    return false;
                }
            }
            if (!setInventoryQuantity(desiredHolder, item, itemState.quantity)) {
                return false;
            }
        } else if (item->owner != nullptr) {
            Object* currentHolder = item->owner;
            inventoryTransferInProgress = true;
            int rc = item_remove_mult(currentHolder, item, static_cast<int>(itemState.quantity));
            if (rc == 0) {
                rc = obj_connect(item, itemState.tile, itemState.elevation, nullptr);
            }
            inventoryTransferInProgress = false;
            if (rc != 0) {
                return false;
            }
        } else if (item->tile < 0) {
            // Move the existing floating node without allocating another one.
            if (obj_move_to_tile(item, itemState.tile, itemState.elevation, nullptr) == -1) {
                return false;
            }
        } else if (item->tile != itemState.tile || item->elevation != itemState.elevation) {
            Rect dirtyRect;
            if (obj_move_to_tile(item, itemState.tile, itemState.elevation, &dirtyRect) == -1) {
                return false;
            }
            tile_refresh_rect(&dirtyRect, itemState.elevation);
        }
    }
    applyVariableState(snapshot);
    set_game_time(snapshot.gameTime);
    if (!worldmap_apply_state(snapshot.worldMap)) {
        std::fprintf(stderr, "Multiplayer snapshot failed world-map state application.\n");
        return false;
    }
    if (!worldmap_apply_travel_progress(snapshot.worldMapTravel.progress)) {
        std::fprintf(stderr, "Multiplayer snapshot failed world-map travel application.\n");
        return false;
    }
    // Native inventory stacking can replace an item body during application.
    // Resolve owners by their final authoritative IDs before setting USED flags.
    for (std::size_t index = 0; index < snapshot.timedEvents.size(); ++index) {
        EntityId id = snapshot.timedEvents[index].ownerId;
        preparedTimerOwners[index] = isValid(id) ? session.entities().findObject(id) : nullptr;
        if (isValid(id) && preparedTimerOwners[index] == nullptr) return false;
    }
    if (!preparedTimers.rebindOwners(preparedTimerOwners) || !preparedTimers.commit()) {
        std::fprintf(stderr, "Multiplayer snapshot failed timed-event application.\n");
        return false;
    }
    if (snapshot.worldMapTravel.stage == WorldMapTravelStage::Proposed) {
        if (activeSharedModal.has_value() && activeSharedModal->kind == SharedModalKind::WorldMap) {
            activeSharedModal.reset();
        }
        selectedWorldMapRoute.reset();
        approvedWorldMapProposerActorId = {};
        approvedWorldMapProposalSequence = {};
        pendingWorldMapProposal = PendingWorldMapProposal {
            snapshot.worldMapTravel.proposerActorId,
            map_data.field_34,
            snapshot.phaseRevision,
            {},
            std::chrono::steady_clock::now() + kWorldMapProposalLifetime,
            {},
        };
    } else if (snapshot.worldMapTravel.stage == WorldMapTravelStage::Approved) {
        pendingWorldMapProposal.reset();
        approvedWorldMapProposalSequence = {};
        activeSharedModal = ActiveSharedModal {
            snapshot.worldMapTravel.controllerActorId,
            SharedModalKind::WorldMap,
        };
        approvedWorldMapProposerActorId = snapshot.worldMapTravel.proposerActorId;
        if (snapshot.worldMapTravel.targetX >= 0) {
            selectedWorldMapRoute = std::make_pair(
                snapshot.worldMapTravel.targetX,
                snapshot.worldMapTravel.targetY);
        } else {
            selectedWorldMapRoute.reset();
        }
    } else {
        pendingWorldMapProposal.reset();
        approvedWorldMapProposerActorId = {};
        approvedWorldMapProposalSequence = {};
        selectedWorldMapRoute.reset();
        if (activeSharedModal.has_value()
            && (activeSharedModal->kind == SharedModalKind::WorldMap
                || sharedModalPhase(activeSharedModal->kind) != snapshot.phase)) {
            activeSharedModal.reset();
        }
    }
    if (localPlayerState() != nullptr) intface_select_item(localPlayerState()->build.activeHand);
    intface_redraw();
    return true;
}

bool networkWorldCaptureAuthoritativeState(EventSequence lastIncludedEvent, WorldSnapshot& snapshot)
{
    return networkWorldCaptureSnapshot(lastIncludedEvent, snapshot);
}

bool networkWorldApplyAuthoritativeState(const WorldSnapshot& snapshot)
{
    return networkWorldApplySnapshot(snapshot, true);
}

std::optional<EntityId> networkWorldFindEntity(const Object* object)
{
    if (!session.isActive() || object == nullptr) {
        return std::nullopt;
    }
    return session.entities().findEntity(object);
}

std::optional<PlayerId> networkWorldCombatOwner(const Object* actor)
{
    if (!session.isActive() || actor == nullptr) {
        return std::nullopt;
    }
    std::optional<EntityId> actorId = session.entities().findEntity(actor);
    if (!actorId.has_value()) {
        return std::nullopt;
    }
    const PlayerCharacterState* player = session.players().findByActor(*actorId);
    return player != nullptr ? std::optional<PlayerId>(player->id) : std::nullopt;
}

void networkWorldExecutePendingCombatStart()
{
    if (worldMode != NetworkLaunchMode::Host || !pendingCombatStart.has_value() || pendingCombatStart->started) return;
    if (session.phase() != SessionPhase::Exploration || isInCombat()) {
        pendingCombatStart.reset();
        return;
    }
    Object* actor = session.entities().findObject(pendingCombatStart->actorId);
    Object* target = isValid(pendingCombatStart->command.targetId)
        ? session.entities().findObject(pendingCombatStart->command.targetId) : nullptr;
    if (actor == nullptr || !critter_is_active(actor)
        || (isValid(pendingCombatStart->command.targetId)
            && (target == nullptr || !critter_is_active(target) || actor->elevation != target->elevation))) {
        pendingCombatStart.reset();
        return;
    }
    pendingCombatStart->started = true;
    STRUCT_664980 request {};
    request.attacker = actor;
    request.defender = target;
    request.maxDamage = std::numeric_limits<int>::max();
    combat(&request);
    pendingCombatStart.reset();
}

bool networkWorldCombatRunInitialAttack(Object* actor)
{
    if (worldMode != NetworkLaunchMode::Host || !pendingCombatStart.has_value()
        || !pendingCombatStart->started
        || session.entities().findObject(pendingCombatStart->actorId) != actor) return false;
    auto pending = *pendingCombatStart;
    pendingCombatStart.reset();
    if (!isValid(pending.command.targetId)) return true;
    Object* target = session.entities().findObject(pending.command.targetId);
    auto* player = session.players().findByActor(pending.actorId);
    if (target == nullptr || player == nullptr) return true;
    ScopedActingPlayerContext context(*player, actor);
    ScopedPlayerFeedback feedback(actor);
    AttackCommand attack { pending.command.targetId, pending.command.hitMode,
        pending.command.hitLocation, combatTurns.revision() };
    if (commandExecutor.attack(actor, target, attack) == CommandExecutionStatus::Applied) {
        deferredEvents.push_back(GameEvent { {}, pending.causedBy,
            AttackStartedEvent { pending.actorId, pending.command.targetId,
                pending.command.hitMode, pending.command.hitLocation } });
    } else {
        char message[] = "The initial attack is no longer possible.";
        display_print(message);
    }
    return true;
}

bool networkWorldCombatBeginRound(Object* const* actors, int count)
{
    if (worldMode != NetworkLaunchMode::Host || !session.isActive()
        || session.phase() != SessionPhase::Combat || actors == nullptr || count <= 0
        || count > static_cast<int>(kMaximumCombatInitiative)) {
        return false;
    }
    openInventories.clear();
    openLootTurns.clear();
    std::vector<CombatTurnEntry> order;
    order.reserve(count);
    for (int index = 0; index < count; index++) {
        Object* actor = actors[index];
        if (actor == nullptr) {
            return false;
        }
        std::optional<EntityId> actorId = session.entities().findEntity(actor);
        if (!actorId.has_value()) {
            EntityRegistrationResult registration = session.registerWorldObject(actor);
            if (!registration) {
                return false;
            }
            actorId = registration.entityId;
            worldCritters.emplace_back(*actorId, actor);
        }
        order.push_back({ *actorId, networkWorldCombatOwner(actor) });
    }
    std::uint64_t revision = combatTurns.active() ? combatTurns.revision() + 1 : 1;
    std::uint64_t round = combatTurns.active() ? combatTurns.round() + 1 : 1;
    if (combatTurns.begin(std::move(order), combatClockMilliseconds(),
            kCombatTurnDurationMilliseconds, revision, round)
        != CombatTurnResult::Accepted) {
        return false;
    }
    queueCombatTurnState();
    return true;
}

bool networkWorldCombatTurnMatches(const Object* actor)
{
    if (!session.isActive()) {
        return true;
    }
    std::optional<EntityId> actorId = session.entities().findEntity(actor);
    return actorId.has_value() && combatTurns.current() != nullptr
        && combatTurns.current()->actorId == *actorId;
}

bool networkWorldCombatActionResolving()
{
    return combatActionResolving;
}

void networkWorldCombatCompleteTurn(Object* actor, std::uint64_t expectedRevision)
{
    if (worldMode != NetworkLaunchMode::Host
        || expectedRevision == 0
        || combatTurns.revision() != expectedRevision
        || !networkWorldCombatTurnMatches(actor)) {
        return;
    }
    std::optional<EntityId> actorId = session.entities().findEntity(actor);
    if (!actorId.has_value()) {
        return;
    }
    const CombatTurnEntry* turn = combatTurns.current();
    std::uint64_t now = combatClockMilliseconds();
    CombatTurnResult result = turn->owner.has_value()
        ? combatTurns.passPlayerTurn(*turn->owner, *actorId, combatTurns.revision(), now)
        : combatTurns.endAiTurn(*actorId, combatTurns.revision(), now);
    if (result == CombatTurnResult::Accepted) {
        queueCombatTurnState();
    }
}

void networkWorldCombatSetPlayerConnected(PlayerId playerId, bool connected)
{
    if (worldMode == NetworkLaunchMode::Host && combatTurns.active()) {
        combatTurns.setConnected(playerId, connected);
        if (!connected) {
            openInventories.erase(session.playerActorId(playerId));
            openLootTurns.erase(session.playerActorId(playerId));
        }
    }
}

void networkWorldCombatTick()
{
    if (worldMode != NetworkLaunchMode::Host || session.phase() != SessionPhase::Combat
        || combatTurns.current() == nullptr || !combatTurns.current()->owner.has_value()) {
        return;
    }
    CombatTurnEntry active = *combatTurns.current();
    std::uint64_t revision = combatTurns.revision();
    std::uint64_t now = combatClockMilliseconds();
    CombatTurnResult result = combatTurns.passDisconnectedPlayer(*active.owner,
        active.actorId, revision, now);
    if (result == CombatTurnResult::StillConnected) {
        result = combatTurns.expirePlayerTurn(*active.owner, active.actorId,
            revision, now);
    }
    if (result == CombatTurnResult::Accepted) {
        queueCombatTurnState();
    }
}

void networkWorldCombatStop()
{
    if (!combatTurns.active()) {
        return;
    }
    combatTurns.stop();
    openInventories.clear();
    openLootTurns.clear();
    queueCombatTurnState();
}

std::optional<PlayerId> networkWorldActiveCombatOwner()
{
    const CombatTurnEntry* turn = combatTurns.current();
    return session.phase() == SessionPhase::Combat && turn != nullptr
        ? turn->owner : std::nullopt;
}

std::uint64_t networkWorldCombatTurnRevision()
{
    return combatTurns.revision();
}

bool networkWorldApplyPeerCombatTurn(const CombatTurnStateChangedEvent& event)
{
    if (worldMode != NetworkLaunchMode::Join || !session.isActive()
        || !isValidCombatTurnState(event.state, event.phase)
        || event.phaseRevision == 0
        || (event.phase != SessionPhase::Combat
            && event.phase != SessionPhase::Exploration)) {
        return false;
    }
    if (event.phaseRevision < session.phaseRevision()) {
        return true;
    }
    if (event.phase == SessionPhase::Combat
        && session.phase() == SessionPhase::Combat
        && event.phaseRevision == session.phaseRevision()
        && event.state.revision < combatTurns.revision()) {
        return true;
    }
    if (session.applyAuthoritativePhase(event.phase, event.phaseRevision)
        != LocalSessionError::None) {
        return false;
    }
    if (event.phase == SessionPhase::Exploration || event.state.initiative.empty()) {
        combatTurns.stop();
        openInventories.clear();
        openLootTurns.clear();
    } else if (!combatTurns.restore(event.state, combatClockMilliseconds())) {
        return false;
    }
    intface_redraw();
    return true;
}

bool networkWorldApplyPeerCombatAction(const CombatActionResolvedEvent& event)
{
    if (worldMode != NetworkLaunchMode::Join || !session.isActive()
        || session.phase() != SessionPhase::Combat
        || !isValid(event.kind) || event.turnRevision == 0
        || event.phaseRevision != session.phaseRevision()) {
        return false;
    }
    return session.players().findByActor(event.actorId) != nullptr
        && session.entities().findObject(event.actorId) != nullptr;
}

bool networkWorldApplyPeerPartyExperience(const PartyExperienceAwardedEvent& event)
{
    if (worldMode != NetworkLaunchMode::Join || !session.isActive()
        || event.amount == 0
        || event.players.size() != session.players().size()) {
        return false;
    }
    std::unordered_set<PlayerId, PlayerIdHash> seen;
    for (const PlayerProgressionResult& result : event.players) {
        const PlayerCharacterState* player = session.players().find(result.playerId);
        if (player == nullptr || player->actorId != result.actorId
            || result.experience < 0 || result.level < 1
            || result.unspentSkillPoints < 0
            || !seen.insert(result.playerId).second) {
            return false;
        }
    }
    // Applying Fallout's XP routine here would repeat level-up rules. The
    // following checkpoint commits the complete builds on the replica.
    return true;
}

Object* networkWorldFindObject(EntityId entityId)
{
    return session.isActive() && isValid(entityId)
        ? session.entities().findObject(entityId)
        : nullptr;
}

Object* networkWorldPlayerActor(PlayerId playerId)
{
    if (!session.isActive()) {
        return nullptr;
    }
    return session.entities().findObject(session.playerActorId(playerId));
}

bool networkWorldPartyDefeated()
{
    if (!session.isActive()) return false;
    bool hasPlayer = false;
    bool allKnockedOut = true;
    for (PlayerId playerId : session.players().playerIds()) {
        Object* actor = networkWorldPlayerActor(playerId);
        // A missing body is a recovery/binding error, not proof of death.
        if (actor == nullptr) {
            allKnockedOut = false;
            continue;
        }
        hasPlayer = true;
        if (critter_is_dead(actor)) return true;
        if ((actor->data.critter.combat.results & DAM_KNOCKED_OUT) == 0) {
            allKnockedOut = false;
        }
    }
    return hasPlayer && allKnockedOut;
}

MultiplayerSaveError networkWorldCaptureMultiplayerSave(
    std::uint64_t generation,
    std::uint64_t saveDatDigest,
    const ReconnectToken& guestReconnectToken,
    MultiplayerSaveSidecar& sidecar)
{
    if (worldMode != NetworkLaunchMode::Host || !session.isActive()) {
        return MultiplayerSaveError::PlayerMissing;
    }
    if (storyPresentation.active) return MultiplayerSaveError::InvalidActivity;
    MultiplayerSaveSidecar captured;
    MultiplayerSaveError error = captureMultiplayerSave(session.players(),
        generation, saveDatDigest, captured);
    if (error != MultiplayerSaveError::None) return error;
    captured.lootDistribution = lootDistribution.state();
    captured.sharedActivity.assign(sharedActivity.begin(),
        sharedActivity.end());
    for (SavedPlayerCharacter& player : captured.players) {
        if (player.playerId == kGuestPlayerId) {
            player.reconnectToken = guestReconnectToken;
            player.replacementAllowed = true;
        }
    }
    Object* host = networkWorldPlayerActor(kHostPlayerId);
    Object* guest = networkWorldPlayerActor(kGuestPlayerId);
    for (const auto& entry : worldItems) {
        Object* top = topEnvironmentOrSelf(entry.second);
        PlayerId owner = top == host ? kHostPlayerId
            : top == guest ? kGuestPlayerId : PlayerId {};
        if (isValid(owner)) {
            captured.ownership.push_back({ entry.first, owner });
        }
    }
    std::sort(captured.ownership.begin(), captured.ownership.end(),
        [](const SavedEntityOwnership& lhs,
            const SavedEntityOwnership& rhs) {
            return lhs.entityId.value < rhs.entityId.value;
        });
    error = validateMultiplayerSave(captured);
    if (error == MultiplayerSaveError::None) sidecar = std::move(captured);
    return error;
}

bool networkWorldApplyPeerDirectTrade(
    const DirectTradeStateChangedEvent& event)
{
    if (!networkWorldReplicaSessionActive()
        || session.entities().findObject(event.actorId) == nullptr
        || session.applyAuthoritativePhase(event.phase, event.phaseRevision)
            != LocalSessionError::None) {
        return false;
    }

    if (event.state.status == DirectTradeStatus::Negotiating
        || event.state.status == DirectTradeStatus::ReadyToCommit) {
        if (event.phase != SessionPhase::Dialogue
            || directTradeController.restore(event.state)
                != DirectTradeResult::Accepted) {
            return false;
        }
        activeSharedModal = ActiveSharedModal {
            event.state.participants[0].actorId,
            SharedModalKind::Barter,
        };
    } else if (event.state.status == DirectTradeStatus::Committed
        || event.state.status == DirectTradeStatus::Cancelled) {
        if (event.phase != SessionPhase::Exploration) return false;
        directTradeController.clear();
        npcBarterState.reset();
        pendingScriptedNpcBarter.reset();
        activeSharedModal.reset();
    } else {
        return false;
    }
    return true;
}

void networkWorldDirectTradeSetConnected(PlayerId playerId, bool connected)
{
    if (!connected && npcBarterState.has_value() && npcBarterState->buyerId == session.playerActorId(playerId)) {
        NpcBarterState state = *npcBarterState;
        npcBarterState.reset();
        pendingScriptedNpcBarter.reset();
        if (worldMode == NetworkLaunchMode::Host) {
            dialogueVotes.deferDeadlineUntil(combatClockMilliseconds() + 60000);
            state.status = NpcBarterStatus::Cancelled;
            state.revision = nextNpcBarterRevision++;
            deferredEvents.push_back(GameEvent { {}, {}, NpcBarterStateChangedEvent { state, session.phaseRevision() } });
        }
    }
    if (!connected) {
        Object* actor = networkWorldPlayerActor(playerId);
        theftAccess.erase(actor);
        activeLootTargets.erase(actor);
    }
    if (worldMode != NetworkLaunchMode::Host || connected
        || !directTradeController.active()) return;
    DirectTradeState state = directTradeController.state();
    if (directTradeController.disconnect(playerId) != DirectTradeResult::Accepted
        || session.transitionTo(SessionPhase::Exploration)
            != LocalSessionError::None) return;
    state = directTradeController.state();
    activeSharedModal.reset();
    EntityId actorId = session.playerActorId(playerId);
    deferredEvents.push_back(GameEvent { {}, {},
        DirectTradeStateChangedEvent { actorId, std::move(state),
            session.phase(), session.phaseRevision(), false } });
}

bool networkWorldRequestScriptedNpcBarter(Object* seller)
{
    Object* buyer = actingPlayerActorOr(obj_dude);
    auto buyerId = session.entities().findEntity(buyer);
    auto sellerId = session.entities().findEntity(seller);
    auto* player = playerStateForActor(buyer);
    if (worldMode != NetworkLaunchMode::Host || session.phase() != SessionPhase::Dialogue
        || !activeSharedModal.has_value() || activeSharedModal->kind != SharedModalKind::Dialogue
        || !buyerId.has_value() || !sellerId.has_value() || player == nullptr
        || player->connection == ConnectionState::Disconnected || activeSharedModal->actorId != *buyerId
        || npcBarterState.has_value() || pendingScriptedNpcBarter.has_value()) return false;
    pendingScriptedNpcBarter = std::make_pair(*buyerId, *sellerId);
    return true;
}

void networkWorldProcessScriptedNpcBarter()
{
    if (worldMode != NetworkLaunchMode::Host || !pendingScriptedNpcBarter.has_value()) return;
    if (session.phase() != SessionPhase::Dialogue) { pendingScriptedNpcBarter.reset(); return; }
    if (!dialoguePresentation.has_value()) return;
    auto request = *pendingScriptedNpcBarter;
    pendingScriptedNpcBarter.reset();
    Object* buyer = session.entities().findObject(request.first);
    auto* player = playerStateForActor(buyer);
    if (player == nullptr || player->connection == ConnectionState::Disconnected
        || dialoguePresentation->actorId != request.first || dialoguePresentation->targetId != request.second) return;
    ScopedActingPlayerContext context(*player, buyer);
    auto result = commandExecutor.npcBarter(buyer, NpcBarterCommand { NpcBarterAction::Begin, request.second });
    if (result.status == CommandExecutionStatus::Applied)
        deferredEvents.push_back(GameEvent { {}, dialogueCause,
            NpcBarterStateChangedEvent { std::move(result.state), session.phaseRevision() } });
}

const NpcBarterState* networkWorldNpcBarterState()
{
    return npcBarterState.has_value() ? &*npcBarterState : nullptr;
}

bool networkWorldNpcBarterItemAvailable(Object* owner, Object* item, bool seller)
{
    return npcBarterItemAvailable(owner, item, seller);
}

bool networkWorldApplyPeerNpcBarter(const NpcBarterStateChangedEvent& event)
{
    if (session.phase() != SessionPhase::Dialogue || session.phaseRevision() != event.phaseRevision
        || session.entities().findObject(event.state.buyerId) == nullptr
        || session.entities().findObject(event.state.sellerId) == nullptr
        || session.players().findByActor(event.state.buyerId) == nullptr) return false;
    if (event.state.status == NpcBarterStatus::Committed || event.state.status == NpcBarterStatus::Cancelled) {
        npcBarterState.reset();
        pendingScriptedNpcBarter.reset();
    } else npcBarterState = event.state;
    return true;
}

std::optional<DirectTradeState> networkWorldDirectTradeState()
{
    return directTradeController.active()
        ? std::optional<DirectTradeState>(directTradeController.state())
        : std::nullopt;
}

void networkWorldLeave()
{
    pendingCombatStart.reset();
    combatActionResolving = false;
    combatTurns.stop();
    openInventories.clear();
    openLootTurns.clear();
    session.stop();
    intExtraResetDialogueActors();
    worldDoors.clear();
    worldScenery.clear();
    worldExitGrids.clear();
    worldItems.clear();
    worldCritters.clear();
    reservedPickupTargets.clear();
    pendingPickups.clear();
    deferredEvents.clear();
    sharedActivity.clear();
    nextSharedActivityId = 1;
    observedFirstVisits = 0;
    dialogueVotes.clear();
    dialoguePresentation.reset();
    directTradeController.clear();
    npcBarterState.reset();
    pendingScriptedNpcBarter.reset();
    lootDistribution.clear();
    pendingTalk.reset();
    dialogueCause = {};
    nextDialogueRevision = 1;
    activeLootTargets.clear();
    theftAccess.clear();
    activeSharedModal.reset();
    approvedWorldMapProposerActorId = {};
    approvedWorldMapProposalSequence = {};
    selectedWorldMapRoute.reset();
    pendingWorldMapProposal.reset();
    worldmap_authoritative_travel_cancel();
    worldMapDeparted = false;
    worldMapOriginX = -1;
    worldMapOriginY = -1;
    itemDropInProgress = false;
    itemUseInProgress = false;
    scriptedSceneryTransitionInProgress = false;
    pendingDialogueMapTransition.reset();
    capturedSceneryMapTransition.reset();
    pendingRestProposal.reset();
    expectedSplitEntityId = {};
    lastSplitEntityId = {};
    storyPresentation = {};
    worldMode = NetworkLaunchMode::Disabled;
    erasePeerActor();
}

bool networkWorldInventoryTransferInProgress()
{
    return inventoryTransferInProgress;
}

bool networkWorldItemDropInProgress()
{
    return itemDropInProgress;
}

bool networkWorldRunCompanionCleanupSmoke(Object* retained)
{
    if (worldMode != NetworkLaunchMode::Host || retained == nullptr
        || retained->pid != 0x100004C || !isPartyMember(retained)) return false;
    const int retainedSid = retained->sid;
    const int retainedHp = critter_get_hits(retained);
    const int retainedCaps = item_caps_total(retained);
    Object* retainedWeapon = inven_right_hand(retained);
    if (retainedWeapon == nullptr) return false;
    const int retainedAmmo = retainedWeapon != nullptr ? item_w_curr_ammo(retainedWeapon) : -1;
    auto retainedId = networkWorldFindEntity(retained);
    auto timersFor = [](const std::vector<QueueEventState>& events, Object* owner) {
        std::vector<QueueEventState> result;
        for (const auto& event : events) if (event.owner == owner) result.push_back(event);
        return result;
    };
    auto sameTimers = [](const auto& first, const auto& second) {
        if (first.size() != second.size()) return false;
        for (std::size_t index = 0; index < first.size(); ++index) {
            const auto& a = first[index]; const auto& b = second[index];
            if (a.time != b.time || a.eventType != b.eventType || a.owner != b.owner
                || a.payloadCount != b.payloadCount || a.payload != b.payload) return false;
        }
        return true;
    };
    for (bool sharedSid : { false, true }) {
        Object* orphan = nullptr;
        Object* bag = nullptr;
        Object* flare = nullptr;
        Object* rounds = nullptr;
        const char* mode = sharedSid ? "shared" : "distinct";
        auto fail = [&](const char* gate) {
            std::fprintf(stderr, "NATIVE_COMPANION_CLEANUP_FAIL sid_mode=%s gate=%s\n", mode, gate);
            return false;
        };
        // Deliberately construct a duplicate native companion and nested
        // resources. Recruitment of the retained Ian happened in real dialogue.
        if (obj_pid_new(&orphan, retained->pid) != 0 || orphan == nullptr
            || obj_move_to_tile(orphan, 0, 0, nullptr) != 0
            || (orphan->sid == -1 && obj_new_sid_inst(orphan, SCRIPT_TYPE_CRITTER, retained->field_80) != 0)
            || obj_pid_new(&bag, 211) != 0 || bag == nullptr
            || obj_disconnect(bag, nullptr) != 0
            || item_add_force(orphan, bag, 1) != 0
            || obj_pid_new(&flare, PROTO_ID_FLARE) != 0 || flare == nullptr
            || obj_disconnect(flare, nullptr) != 0
            || item_add_force(bag, flare, 1) != 0
            || protinst_use_item(orphan, flare) != 0
            || !queue_find(flare, EVENT_TYPE_FLARE)
            || obj_pid_new(&rounds, 29) != 0 || rounds == nullptr
            || obj_disconnect(rounds, nullptr) != 0
            || item_add_force(bag, rounds, 3) != 0) return fail("native_setup");
        int ownedSid = orphan->sid;
        Script* ownedScript = nullptr;
        if (ownedSid == -1 || ownedSid == retainedSid || scr_ptr(ownedSid, &ownedScript) != 0
            || ownedScript->owner != orphan) return fail("owned_script");
        if (sharedSid) {
            if (scr_remove(ownedSid) != 0) return fail("replace_alias");
            orphan->sid = retainedSid;
        }
        auto* timer = static_cast<ScriptEvent*>(mem_malloc(sizeof(ScriptEvent)));
        if (timer == nullptr) return fail("timer_allocation");
        timer->sid = orphan->sid;
        timer->fixedParam = -701;
        if (queue_add(1000000, orphan, timer, EVENT_TYPE_SCRIPT) != 0
            || queue_add(1000000, orphan, nullptr, EVENT_TYPE_POISON) != 0) return fail("orphan_timers");
        WorldSnapshot before;
        if (!networkWorldCaptureAuthoritativeState({}, before)) return fail("setup_capture");
        auto orphanId = networkWorldFindEntity(orphan);
        auto bagId = networkWorldFindEntity(bag);
        auto flareId = networkWorldFindEntity(flare);
        auto roundsId = networkWorldFindEntity(rounds);
        if (!retainedId || !orphanId || !bagId || !flareId || !roundsId) return fail("native_registration");
        int repeatedVisits = 0;
        for (Object* object = obj_find_first(); object != nullptr; object = obj_find_next())
            if (object == orphan) ++repeatedVisits;
        if (repeatedVisits != 2) return fail("first_tile_replay");
        std::vector<QueueEventState> beforeQueue;
        if (!queue_capture_state(beforeQueue)) return fail("queue_before");
        auto realTimers = timersFor(beforeQueue, retained);
        if (realTimers.empty()) return fail("retained_native_timer");
        DB_FILE* stream = db_fopen("companion-cleanup-party.dat", "wb");
        bool saved = stream != nullptr && partyMemberSave(stream) == 0;
        if (stream != nullptr && db_fclose(stream) != 0) saved = false;
        if (!saved) return fail("party_save");
        stream = db_fopen("companion-cleanup-party.dat", "rb");
        bool loaded = stream != nullptr && partyMemberLoad(stream) == 0;
        if (stream != nullptr && db_fclose(stream) != 0) loaded = false;
        if (!loaded) return fail("party_load");
        // Never dereference the removed pointers after native cleanup.
        std::vector<QueueEventState> afterQueue;
        if (!queue_capture_state(afterQueue)) return fail("queue_after");
        for (const auto& event : afterQueue) {
            if (event.owner == orphan || event.owner == bag || event.owner == flare || event.owner == rounds)
                return fail("dangling_timer");
        }
        if (!sameTimers(realTimers, timersFor(afterQueue, retained))) return fail("retained_timer_changed");
        if (networkWorldFindObject(*orphanId) != nullptr || networkWorldFindObject(*bagId) != nullptr
            || networkWorldFindObject(*flareId) != nullptr || networkWorldFindObject(*roundsId) != nullptr)
            return fail("dangling_native_entity");
        Script* actualScript = nullptr;
        if ((!sharedSid && scr_ptr(ownedSid, &actualScript) == 0)
            || scr_ptr(retainedSid, &actualScript) != 0 || actualScript->owner != retained
            || retained->sid != retainedSid || partyMemberFindObjFromPid(retained->pid) != retained
            || !isPartyMember(retained) || networkWorldFindObject(*retainedId) != retained
            || critter_get_hits(retained) != retainedHp || item_caps_total(retained) != retainedCaps
            || inven_right_hand(retained) != retainedWeapon || item_w_curr_ammo(retainedWeapon) != retainedAmmo)
            return fail("retained_script_or_resources");
        WorldSnapshot after;
        if (!networkWorldCaptureAuthoritativeState({}, after)) return fail("post_cleanup_capture");
        std::fprintf(stderr,
            "NATIVE_COMPANION_CLEANUP_PASS sid_mode=%s first_tile_visits=%d native_party_load=1 orphan_removed=1 nested_ids_removed=1 root_timers_removed=1 child_timer_removed=1 retained_script_owner=1 retained_timer_exact=1 retained_resources=1\n",
            mode, repeatedVisits);
    }
    return true;
}

PartyExperienceResult networkWorldAwardPartyExperience(int xp)
{
    if (!session.isActive() || peerActor == nullptr) {
        return PartyExperienceResult::NotMultiplayer;
    }
    if (worldMode == NetworkLaunchMode::Join) {
        return PartyExperienceResult::ReplicaIgnored;
    }
    if (worldMode != NetworkLaunchMode::Host) {
        return PartyExperienceResult::Failed;
    }

    if (xp == 0) return PartyExperienceResult::Applied;
    std::vector<std::pair<PlayerCharacterState*, Object*>> party;
    for (PlayerId playerId : session.players().playerIds()) {
        PlayerCharacterState* player = session.players().find(playerId);
        Object* actor = player != nullptr ? session.entities().findObject(player->actorId) : nullptr;
        if (player == nullptr || actor == nullptr) {
            return PartyExperienceResult::Failed;
        }
        party.emplace_back(player, actor);
    }

    for (const auto& member : party) {
        ScopedActingPlayerContext actingPlayer(*member.first, member.second);
        if (stat_pc_add_experience(xp) != 0) {
            return PartyExperienceResult::Failed;
        }
    }
    PartyExperienceAwardedEvent award;
    award.actorId = session.playerActorId(kHostPlayerId);
    award.amount = xp;
    for (const auto& member : party) {
        award.players.push_back(PlayerProgressionResult {
            member.first->id,
            member.first->actorId,
            member.first->build.experience,
            member.first->build.level,
            member.first->build.unspentSkillPoints,
        });
    }
    deferredEvents.push_back(GameEvent {
        {}, CommandSequence { 1 }, std::move(award),
    });
    return PartyExperienceResult::Applied;
}

bool networkWorldRunPartyExperienceSmokeTest()
{
    if (!session.isActive()) {
        return false;
    }
    PlayerCharacterState* host = session.players().find(kHostPlayerId);
    PlayerCharacterState* guest = session.players().find(kGuestPlayerId);
    if (host == nullptr || guest == nullptr) {
        return false;
    }

    int hostExperience = host->build.experience;
    int guestExperience = guest->build.experience;
    PartyExperienceResult result = networkWorldAwardPartyExperience(125);
    if (worldMode == NetworkLaunchMode::Host) {
        return result == PartyExperienceResult::Applied
            && host->build.experience == hostExperience + 125
            && guest->build.experience == guestExperience + 125;
    }
    return worldMode == NetworkLaunchMode::Join
        && result == PartyExperienceResult::ReplicaIgnored
        && host->build.experience == hostExperience
        && guest->build.experience == guestExperience;
}

bool networkWorldActive()
{
    return session.isActive() && peerActor != nullptr;
}

bool networkWorldReplicaSessionActive()
{
    return session.isActive() && worldMode == NetworkLaunchMode::Join;
}

} // namespace multiplayer
} // namespace fallout
