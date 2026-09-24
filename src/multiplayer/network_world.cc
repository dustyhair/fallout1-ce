#include "multiplayer/network_world.h"

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
#include "game/anim.h"
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
#include "game/scripts.h"
#include "game/stat.h"
#include "game/tile.h"
#include "game/worldmap.h"
#include "multiplayer/acting_player_context.h"
#include "multiplayer/combat_turn_controller.h"
#include "multiplayer/dialogue_vote_controller.h"
#include "multiplayer/local_player_context.h"
#include "multiplayer/local_session.h"
#include "multiplayer/presentation_bridge.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/input.h"
#include "plib/gnw/memory.h"
#include "plib/gnw/svga.h"

namespace fallout {
namespace multiplayer {
namespace {

LocalSession session;
Object* peerActor = nullptr;
CommandProcessor commandProcessor;
CombatTurnController combatTurns;
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
std::optional<DialoguePresentationEvent> dialoguePresentation;
struct PendingTalk {
    EntityId actorId;
    EntityId targetId;
    CommandSequence causedBy;
};
std::optional<PendingTalk> pendingTalk;
CommandSequence dialogueCause;
std::uint64_t nextDialogueRevision = 1;
std::deque<SharedActivityEntry> sharedActivity;
std::uint64_t nextSharedActivityId = 1;
std::uint32_t observedFirstVisits = 0;

std::unordered_map<Object*, Object*> activeLootTargets;
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
    if (map == 0) {
        // Fallout's world-map exit has no destination map placement yet.
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
    Rect dirtyRect {};
    if ((object->fid != fid && obj_change_fid(object, fid, &dirtyRect) == -1)
        || (object->frame != frame && obj_set_frame(object, frame, &dirtyRect) == -1)) {
        return false;
    }
    if (object->lightDistance != lightDistance || object->lightIntensity != lightIntensity) {
        obj_set_light(object, lightDistance, lightIntensity, &dirtyRect);
    }
    object->flags = static_cast<int>((static_cast<std::uint32_t>(object->flags) & ~kSharedObjectFlagMask)
        | objectFlags);
    tile_refresh_rect(&dirtyRect, object->elevation);
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

bool validateVariableState(const WorldSnapshot& snapshot)
{
    return num_game_global_vars >= 0
        && num_map_global_vars >= 0
        && num_map_local_vars >= 0
        && snapshot.gameGlobalVariables.size() == static_cast<std::size_t>(num_game_global_vars)
        && snapshot.mapGlobalVariables.size() == static_cast<std::size_t>(num_map_global_vars)
        && snapshot.mapLocalVariables.size() == static_cast<std::size_t>(num_map_local_vars)
        && (num_game_global_vars == 0 || game_global_vars != nullptr)
        && (num_map_global_vars == 0 || map_global_vars != nullptr)
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
    if (!queue_capture_state(queueEvents) || queueEvents.size() > kMaxSnapshotTimedEvents) {
        return false;
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
                return false;
            }
            event.ownerId = *ownerId;
        }
        snapshot.timedEvents.push_back(event);
    }
    return true;
}

bool applyTimedEvents(const WorldSnapshot& snapshot)
{
    std::vector<QueueEventState> queueEvents;
    queueEvents.reserve(snapshot.timedEvents.size());
    for (const TimedEventSnapshot& event : snapshot.timedEvents) {
        QueueEventState queueEvent;
        queueEvent.time = event.time;
        queueEvent.eventType = event.eventType;
        queueEvent.payloadCount = event.payloadCount;
        for (std::size_t index = 0; index < event.payload.size(); index++) {
            queueEvent.payload[index] = event.payload[index];
        }
        if (isValid(event.ownerId)) {
            queueEvent.owner = session.entities().findObject(event.ownerId);
            if (queueEvent.owner == nullptr) {
                return false;
            }
        }
        queueEvents.push_back(queueEvent);
    }
    return queue_replace_state(queueEvents);
}

bool validateActorState(const WorldSnapshot& snapshot)
{
    if (snapshot.actors.size() != 2) {
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

bool applyActorAndCritterState(const WorldSnapshot& snapshot)
{
    for (const ActorSnapshot& actorState : snapshot.actors) {
        Object* actor = session.entities().findObject(actorState.entityId);
        if (session.players().setBuild(actorState.ownerId, actorState.build) != PlayerStateError::None) {
            std::fprintf(stderr, "Snapshot actor build failed id=%u owner=%u.\n",
                actorState.entityId.value, actorState.ownerId.value);
            return false;
        }
        Rect dirtyRect {};
        bool dirty = false;
        if (actor->tile != actorState.tile || actor->elevation != actorState.elevation) {
            register_clear(actor);
            if (obj_move_to_tile(actor, actorState.tile, actorState.elevation, &dirtyRect) == -1) {
                std::fprintf(stderr, "Snapshot actor move failed id=%u from=%d to=%d elev=%d.\n",
                    actorState.entityId.value, actor->tile, actorState.tile,
                    actorState.elevation);
                return false;
            }
            dirty = true;
        }
        if (actor->rotation != actorState.rotation) {
            if (obj_set_rotation(actor, actorState.rotation, &dirtyRect) == -1) {
                std::fprintf(stderr, "Snapshot actor rotation failed id=%u.\n",
                    actorState.entityId.value);
                return false;
            }
            dirty = true;
        }
        if (!applySharedObjectPresentation(actor, actorState.fid,
                actorState.frame, actorState.objectFlags,
                actorState.lightDistance, actorState.lightIntensity)) {
            std::fprintf(stderr, "Snapshot actor presentation failed id=%u.\n",
                actorState.entityId.value);
            return false;
        }
        // A replica applies the host's selected death and animation state; it
        // must never call critter_kill through critter_adjust_hits here.
        actor->data.critter.hp = actorState.hitPoints;
        actor->data.critter.combat.ap = actorState.actionPoints;
        actor->data.critter.combat.results = actorState.combatResults;
        actor->data.critter.combat.maneuver = actorState.combatManeuver;
        actor->data.critter.combat.damageLastTurn = actorState.damageLastTurn;
        actor->data.critter.combat.team = actorState.team;
        if (dirty) {
            tile_refresh_rect(&dirtyRect, actorState.elevation);
        }
    }
    for (const CritterSnapshot& critterState : snapshot.critters) {
        Object* critter = session.entities().findObject(critterState.entityId);
        Rect dirtyRect {};
        bool dirty = false;
        if (critter->tile != critterState.tile || critter->elevation != critterState.elevation) {
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
        if (dirty) {
            tile_refresh_rect(&dirtyRect, critterState.elevation);
        }
    }
    for (const ActorSnapshot& actorState : snapshot.actors) {
        Object* actor = session.entities().findObject(actorState.entityId);
        actor->data.critter.combat.whoHitMe = isValid(actorState.whoHitMeId)
            ? session.entities().findObject(actorState.whoHitMeId) : nullptr;
        actor->data.critter.combat.whoHitMeCid =
            actor->data.critter.combat.whoHitMe != nullptr
            ? actor->data.critter.combat.whoHitMe->cid : -1;
    }
    for (const CritterSnapshot& critterState : snapshot.critters) {
        Object* critter = session.entities().findObject(critterState.entityId);
        critter->data.critter.combat.whoHitMe = isValid(critterState.whoHitMeId)
            ? session.entities().findObject(critterState.whoHitMeId) : nullptr;
        critter->data.critter.combat.whoHitMeCid =
            critter->data.critter.combat.whoHitMe != nullptr
            ? critter->data.critter.combat.whoHitMe->cid : -1;
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

bool hasDirectItemWithDescriptor(const Object* holder, const ItemDescriptor& descriptor)
{
    if (holder == nullptr) {
        return false;
    }
    const Inventory& inventory = holder->data.inventory;
    for (int index = 0; index < inventory.length; index++) {
        ItemDescriptor candidateDescriptor;
        if (describeItem(inventory.items[index].item, candidateDescriptor)
            && itemDescriptorsEqual(candidateDescriptor, descriptor)) {
            return true;
        }
    }
    return false;
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

bool lootTargetIsInRange(Object* actor, Object* target)
{
    return actor != nullptr
        && target != nullptr
        && actor != target
        && FID_TYPE(target->fid) == OBJ_TYPE_CRITTER
        && actor->elevation == target->elevation
        && obj_dist(actor, target) == 1;
}

bool isPlayerActor(Object* actor)
{
    std::optional<EntityId> actorId = session.entities().findEntity(actor);
    return actorId.has_value() && session.players().findByActor(*actorId) != nullptr;
}

bool isAdjacentPlayerActor(Object* actor, Object* target)
{
    if (!lootTargetIsInRange(actor, target)) {
        return false;
    }
    return isPlayerActor(target);
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
        || item_count(source, item) < static_cast<int>(quantity)) {
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
        || (quantity > 1 && item->pid != PROTO_ID_MONEY)) {
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

bool loadSharedMap(int map)
{
    if (!session.isActive()
        || session.phase() != SessionPhase::Transition
        || peerActor == nullptr
        || map < 0) {
        return false;
    }

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
    deferredEvents.clear();
    dialogueVotes.clear();
    dialoguePresentation.reset();
    pendingTalk.reset();
    dialogueCause = {};
    nextDialogueRevision = 1;
    activeLootTargets.clear();
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
    if (session.rebindPlayerActor(peerPlayerId, replacement) != LocalSessionError::None) {
        obj_erase_object(replacement, nullptr);
        return false;
    }
    peerActor = replacement;
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
            if (queueResult != 0 || game_user_wants_to_quit != 0) {
                std::fprintf(stderr,
                    "Multiplayer rest interrupted by queue at time=%d result=%d quit=%d.\n",
                    game_time(), queueResult, game_user_wants_to_quit);
                interrupted = true;
                break;
            }
        }
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
    return interrupted || (untilHealed && !sharedPlayersHealed());
}

class NetworkCommandExecutor : public CommandExecutor {
public:
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
            pathLength, command.running, 0);
        int committed = register_end();
        if (scheduled == -1 || committed == -1) {
            register_clear(actor);
            return CommandExecutionStatus::InvalidAction;
        }
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
        combatActionResolving = true;
        int result = target != nullptr
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

    DoorUseExecution useDoor(Object* actor, Object* target) override
    {
        DoorUseExecution execution;
        if (isInCombat()
            || target == nullptr
            || actor->elevation != target->elevation
            || !obj_is_a_portal(target)
            || action_use_an_object(actor, target) == -1) {
            return execution;
        }
        execution.status = CommandExecutionStatus::Applied;
        execution.open = obj_is_open(target) != 0;
        execution.locked = obj_is_locked(target);
        execution.frame = target->frame;
        return execution;
    }

    CommandExecutionStatus pickup(Object* actor, Object* target) override
    {
        return beginPickup(actor, target)
            ? CommandExecutionStatus::Applied
            : CommandExecutionStatus::InvalidAction;
    }

    CommandExecutionStatus loot(Object* actor, Object* target) override
    {
        if (isInCombat()
            || isPlayerActor(target)
            || !lootTargetIsInRange(actor, target)) {
            return CommandExecutionStatus::InvalidAction;
        }
        activeLootTargets[actor] = target;
        return CommandExecutionStatus::Applied;
    }

    CommandExecutionStatus useSkill(Object* actor, Object* target, const UseSkillCommand& command) override
    {
        if (isInCombat()
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

        if (destinationMap == 0) {
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

        if (requestedWorldMap || (requestedTransition.has_value() && requestedTransition->map == 0)) {
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
                    display_print("Shared rest was interrupted.");
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
            || combat_check_bad_shot(actor, target, command.hitMode,
                   command.hitLocation != HIT_LOCATION_UNCALLED) != COMBAT_BAD_SHOT_OK
            || combatActionResolving) {
            return CommandExecutionStatus::InvalidAction;
        }
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
                if (worldMapDeparted) {
                    execution.arrival = completeWorldMapTravel(WorldMapArrivalKind::Terrain, 0);
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

    InventoryTransferExecution transferInventory(Object* actor,
        Object* source,
        Object* destination,
        Object* item,
        const InventoryTransferCommand& command) override
    {
        InventoryTransferExecution execution;
        if (actor == nullptr || source == nullptr || destination == nullptr) {
            return execution;
        }
        auto activeLoot = activeLootTargets.find(actor);
        Object* sourceTop = topEnvironmentOrSelf(source);
        Object* destinationTop = topEnvironmentOrSelf(destination);
        Object* otherTop = sourceTop == actor ? destinationTop : sourceTop;
        bool lootTransfer = activeLoot != activeLootTargets.end()
            && activeLoot->second == otherTop
            && !isPlayerActor(otherTop)
            && lootTargetIsInRange(actor, otherTop);
        bool playerGift = item != nullptr
            && source == actor
            && destination == destinationTop
            && isAdjacentPlayerActor(actor, destination)
            && (item->flags & (OBJECT_EQUIPPED | OBJECT_USED)) == 0;
        if (isInCombat()
            || (sourceTop != actor && destinationTop != actor)
            || (!lootTransfer && !playerGift)) {
            return execution;
        }

        bool created = false;
        if (item == nullptr) {
            if (isValid(command.itemId)
                || !hasItemDescriptor(command.itemDescriptor)
                || sourceTop != actor
                || hasDirectItemWithDescriptor(source, command.itemDescriptor)) {
                return execution;
            }
            item = createItem(command.itemDescriptor);
            if (item == nullptr
                || item_add_force(source, item, static_cast<int>(command.sourceQuantity)) != 0) {
                if (item != nullptr) {
                    obj_erase_object(item, nullptr);
                }
                return execution;
            }
            EntityRegistrationResult registration = registerItem(item);
            if (!registration) {
                item_remove_mult(source, item, static_cast<int>(command.sourceQuantity));
                obj_erase_object(item, nullptr);
                return execution;
            }
            execution.itemId = registration.entityId;
            created = true;
        } else {
            execution.itemId = command.itemId;
        }

        if (item_count(source, item) != static_cast<int>(command.sourceQuantity)
            || !describeItem(item, execution.itemDescriptor)
            || !applyInventoryTransfer(source, destination, item, command.quantity, false)) {
            if (created && item->owner == source) {
                session.entities().unregisterEntity(execution.itemId);
                worldItems.erase(std::remove_if(worldItems.begin(), worldItems.end(), [&](const auto& entry) {
                    return entry.first == execution.itemId;
                }), worldItems.end());
                item_remove_mult(source, item, static_cast<int>(command.sourceQuantity));
                obj_erase_object(item, nullptr);
            }
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
            || isInCombat()
            || topEnvironmentOrSelf(source) != actor
            || !hexGridTileIsValid(actor->tile)
            || !elevationIsValid(actor->elevation)) {
            return execution;
        }

        bool created = false;
        if (item == nullptr) {
            if (isValid(command.itemId)
                || !hasItemDescriptor(command.itemDescriptor)
                || hasDirectItemWithDescriptor(source, command.itemDescriptor)) {
                return execution;
            }
            item = createItem(command.itemDescriptor);
            if (item == nullptr
                || item_add_force(source, item, static_cast<int>(command.sourceQuantity)) != 0) {
                if (item != nullptr) {
                    obj_erase_object(item, nullptr);
                }
                return execution;
            }
            EntityRegistrationResult registration = registerItem(item);
            if (!registration) {
                item_remove_mult(source, item, static_cast<int>(command.sourceQuantity));
                obj_erase_object(item, nullptr);
                return execution;
            }
            execution.itemId = registration.entityId;
            created = true;
        } else {
            execution.itemId = command.itemId;
        }

        if (item_count(source, item) != static_cast<int>(command.sourceQuantity)
            || (command.quantity > 1 && item->pid != PROTO_ID_MONEY)
            || !applyItemDrop(source, item, command.quantity)
            || !describeItem(item, execution.itemDescriptor)) {
            if (created && item->owner == source) {
                session.entities().unregisterEntity(execution.itemId);
                worldItems.erase(std::remove_if(worldItems.begin(), worldItems.end(), [&](const auto& entry) {
                    return entry.first == execution.itemId;
                }), worldItems.end());
                item_remove_mult(source, item, static_cast<int>(command.sourceQuantity));
                obj_erase_object(item, nullptr);
            }
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

bool registerWorldObjects()
{
    combatTurns.stop();
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
    for (Object* object = obj_find_first(); object != nullptr; object = obj_find_next()) {
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

    for (Object* exitGrid : exitGrids) {
        EntityRegistrationResult registration = session.registerWorldObject(exitGrid);
        if (!registration) {
            return false;
        }
        worldExitGrids.emplace_back(registration.entityId, exitGrid);
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

bool refreshPlayer(PlayerId playerId)
{
    PlayerCharacterState* player = session.players().find(playerId);
    Object* actor = player != nullptr ? session.entities().findObject(player->actorId) : nullptr;
    if (player == nullptr || actor == nullptr) {
        return false;
    }

    ScopedActingPlayerContext actingPlayer(*player, actor);
    stat_recalc_derived(actor);
    int hitPoints = critter_get_hits(actor);
    int maximumHitPoints = stat_level(actor, STAT_MAXIMUM_HIT_POINTS);
    critter_adjust_hits(actor, maximumHitPoints - hitPoints);
    if (updatePlayerGenderAppearance(actor) == -1) {
        return false;
    }
    dude_stand(actor, actor->rotation, -1);
    return true;
}

} // namespace

bool networkWorldEnter(NetworkLaunchMode mode,
    const CharacterCreationSheet& localSheet,
    const CharacterCreationSheet& peerSheet)
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
        || !refreshPlayer(kHostPlayerId)
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
    if (!session.isActive() || isInCombat()) {
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
        || FID_TYPE(target->fid) != OBJ_TYPE_CRITTER
        || !lootTargetIsInRange(actor, target)) {
        return false;
    }
    if (actor != obj_dude) {
        return true;
    }
    activeLootTargets[actor] = target;
    if (inven_loot_window_is_active()) {
        return true;
    }
    ScopedActingPlayerContext actingPlayer(*player, actor);
    return action_loot_container(actor, target) != -1;
}

bool networkWorldApplyPeerSharedModal(const SharedModalStateChangedEvent& modal)
{
    if (!session.isActive()
        || !isValid(modal.kind)
        || session.entities().findObject(modal.actorId) == nullptr
        || modal.phaseRevision == 0) {
        return false;
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
    // Never rerun combat_attack on a replica: it consumes RNG and computes
    // damage. Authoritative actor and critter values arrive in snapshots.
    return true;
}

bool networkWorldApplyPeerSkillUse(const SkillUseStartedEvent& skillUse)
{
    if (!session.isActive() || isInCombat() || !isValid(skillUse.skill)) {
        return false;
    }
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
            display_print("Shared rest was interrupted.");
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

bool networkWorldRunEngineAuthoritySmokeTest(EngineExecutionProbeCounts& counts)
{
    counts = {};
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
    if (!registerItem(item)) return false;
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
    Object* weapon = nullptr;
    Object* ammo = nullptr;
    if (obj_pid_new(&weapon, weaponPid) == -1 || weapon == nullptr
        || obj_pid_new(&ammo, ammoPid) == -1 || ammo == nullptr) {
        if (weapon != nullptr) obj_erase_object(weapon, nullptr);
        if (ammo != nullptr) obj_erase_object(ammo, nullptr);
        return false;
    }
    if (obj_disconnect(weapon, nullptr) == -1
        || obj_disconnect(ammo, nullptr) == -1
        || item_add_force(guest, weapon, 1) != 0
        || item_add_force(guest, ammo, 1) != 0) {
        return false;
    }
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
            if (lethal) target->data.critter.hp = 1;
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
                && obj_move_to_tile(actor, tile, critter->elevation, nullptr) == 0
                && lootTargetIsInRange(actor, critter)) {
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
        return MapTransitionSmokeFixture {
            elevatorType,
            destinationLevel,
            destinationMap,
            destinationElevation,
            7,
            session.playerActorId(kHostPlayerId),
            session.playerActorId(kGuestPlayerId),
        };
    }
    return std::nullopt;
}

bool networkWorldVerifyMapTransitionSmokeTest(const MapTransitionSmokeFixture& fixture)
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
            if (hexGridTileIsValid(candidate)
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
    bool rejected = outcome.result.status == CommandStatus::Rejected
        && outcome.result.rejection == CommandRejection::InvalidAction
        && !outcome.event.has_value()
        && activeLootTargets.find(actor) == activeLootTargets.end();

    commandProcessor.reset();
    bool restored = obj_move_to_tile(actor, adjacentTile, elevation, nullptr) == 0
        && lootTargetIsInRange(actor, target);
    return rejected && restored;
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
    return std::nullopt;
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
    return rejected && restored && playerLootRejected && gifted && takingRejected && rolledBack;
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
            if (!isExitGrid(exitGrid) || exitGrid->data.misc.map != 0) continue;
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
        || FID_TYPE(target->fid) != OBJ_TYPE_CRITTER
        || obj_dude->elevation != target->elevation) {
        return false;
    }
    activeLootTargets[obj_dude] = target;
    return action_loot_container(obj_dude, target) != -1;
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

bool networkWorldIsLocalInventoryTransfer(Object* source, Object* destination)
{
    if (!session.isActive() || obj_dude == nullptr || source == nullptr || destination == nullptr) {
        return false;
    }
    auto activeLoot = activeLootTargets.find(obj_dude);
    if (activeLoot == activeLootTargets.end()) {
        return false;
    }
    Object* sourceTop = topEnvironmentOrSelf(source);
    Object* destinationTop = topEnvironmentOrSelf(destination);
    return (sourceTop == obj_dude && destinationTop == activeLoot->second)
        || (destinationTop == obj_dude && sourceTop == activeLoot->second);
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
    if (!session.isActive() || object == nullptr || FID_TYPE(object->fid) != OBJ_TYPE_ITEM) {
        return;
    }
    std::optional<EntityId> entityId = session.entities().findEntity(object);
    if (!entityId.has_value()) {
        return;
    }
    session.entities().unregisterEntity(*entityId);
    worldItems.erase(std::remove_if(worldItems.begin(), worldItems.end(), [&](const auto& entry) {
        return entry.first == *entityId;
    }), worldItems.end());
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
    if (source == nullptr || destination == nullptr) {
        return false;
    }
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

    AuthoritativeCommandResult result = commandProcessor.process(command, session, commandExecutor);
    if (result.result.status == CommandStatus::Accepted && !result.replayed
        && result.event.has_value()) {
        if (std::holds_alternative<DialogueRequestedEvent>(result.event->payload)) {
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
            pickup != nullptr && !result.replayed) {
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
    if (worldMode != NetworkLaunchMode::Host || !dialogueVotes.active()) return std::nullopt;
    std::uint64_t now = combatClockMilliseconds();
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
        dialogueVotes.setConnected(playerId, connected);
    }
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
        for (PlayerId playerId : { kHostPlayerId, kGuestPlayerId }) {
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
    return isExitGrid(exitGrid) && exitGrid->data.misc.map == 0;
}

bool networkWorldSharedModalActive()
{
    return activeSharedModal.has_value()
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

bool networkWorldCaptureSnapshot(EventSequence lastIncludedEvent, WorldSnapshot& snapshot)
{
    if (!session.isActive()) {
        return false;
    }

    WorldSnapshot captured;
    captured.lastIncludedEvent = lastIncludedEvent;
    captured.phase = session.phase();
    captured.phaseRevision = session.phaseRevision();
    if (captured.phase == SessionPhase::Combat) {
        captured.combat = combatTurns.snapshot(combatClockMilliseconds());
        captured.combatFreeMove = combat_free_move;
    }
    captured.gameTime = game_time();
    captured.sharedActivity.assign(sharedActivity.begin(), sharedActivity.end());
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
    for (PlayerId playerId : { kHostPlayerId, kGuestPlayerId }) {
        EntityId actorId = session.playerActorId(playerId);
        Object* actor = session.entities().findObject(actorId);
        PlayerCharacterState* player = session.players().find(playerId);
        if (actor == nullptr || player == nullptr || anim_busy(actor) == -1) {
            return false;
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
        });
    }
    for (const auto& entry : worldScenery) {
        Object* scenery = entry.second;
        if (scenery == nullptr
            || session.entities().findObject(entry.first) != scenery
            || anim_busy(scenery) == -1
            || scenery->tile < 0) {
            return false;
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
            return false;
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
        });
    }
    for (const auto& entry : worldDoors) {
        Object* door = entry.second;
        if (door == nullptr
            || session.entities().findObject(entry.first) != door
            || anim_busy(door) == -1) {
            return false;
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
            return false;
        }
        ItemSnapshot itemState;
        itemState.entityId = entry.first;
        if (!describeItem(item, itemState.itemDescriptor)) {
            return false;
        }
        if (item->owner == nullptr) {
            if (item->tile < 0 || !elevationIsValid(item->elevation)) {
                return false;
            }
            itemState.tile = item->tile;
            itemState.elevation = item->elevation;
            itemState.quantity = 1;
        } else {
            std::optional<EntityId> holderId = session.entities().findEntity(item->owner);
            int quantity = item_count(item->owner, item);
            if (!holderId.has_value() || quantity <= 0) {
                return false;
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
    if (!captureVariables(game_global_vars, num_game_global_vars, captured.gameGlobalVariables)
        || !captureVariables(map_global_vars, num_map_global_vars, captured.mapGlobalVariables)
        || !captureVariables(map_local_vars, num_map_local_vars, captured.mapLocalVariables)
        || !captureTimedEvents(captured)) {
        return false;
    }
    if (validateSnapshot(captured) != SnapshotError::None) {
        return false;
    }
    snapshot = std::move(captured);
    return true;
}

bool networkWorldApplySnapshot(const WorldSnapshot& snapshot)
{
    SnapshotError snapshotError = validateSnapshot(snapshot);
    if (session.isActive()
        && snapshotError == SnapshotError::None
        && networkWorldReplicaSessionActive()
        && snapshot.mapLocalVariables.size() > static_cast<std::size_t>(num_map_local_vars)
        && !map_ensure_local_vars(static_cast<int>(snapshot.mapLocalVariables.size()))) {
        return false;
    }
    bool variablesValid = validateVariableState(snapshot);
    if (!session.isActive()
        || snapshotError != SnapshotError::None
        || snapshot.doors.size() != worldDoors.size()
        || snapshot.scenery.size() != worldScenery.size()
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

    if (!validateActorState(snapshot)) {
        std::fprintf(stderr, "Multiplayer snapshot actor identity preflight failed.\n");
        return false;
    }
    for (const DoorSnapshot& doorState : snapshot.doors) {
        Object* door = session.entities().findObject(doorState.entityId);
        if (door == nullptr
            || !obj_is_a_portal(door)
            || doorState.open != (doorState.frame != 0)) {
            return false;
        }
    }
    for (const ScenerySnapshot& sceneryState : snapshot.scenery) {
        Object* scenery = session.entities().findObject(sceneryState.entityId);
        if (scenery == nullptr
            || FID_TYPE(scenery->fid) != OBJ_TYPE_SCENERY
            || scenery->pid != sceneryState.pid) {
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
    if (snapshot.critters.size() != worldCritters.size() || !validateCritterState(snapshot)) {
        std::vector<Object*> localCritters;
        localCritters.reserve(worldCritters.size());
        for (const auto& entry : worldCritters) {
            localCritters.push_back(entry.second);
            session.entities().unregisterEntity(entry.first);
        }
        worldCritters.clear();
        std::unordered_set<Object*> reboundCritters;
        for (const CritterSnapshot& critterState : snapshot.critters) {
            Object* matched = nullptr;
            for (Object* candidate : localCritters) {
                if (candidate != nullptr
                    && reboundCritters.find(candidate) == reboundCritters.end()
                    && candidate->pid == critterState.pid
                    && candidate->tile == critterState.tile
                    && candidate->elevation == critterState.elevation) {
                    matched = candidate;
                    break;
                }
            }
            if (matched == nullptr) {
                Object* solePidMatch = nullptr;
                for (Object* candidate : localCritters) {
                    if (candidate != nullptr
                        && reboundCritters.find(candidate) == reboundCritters.end()
                        && candidate->pid == critterState.pid) {
                        if (solePidMatch != nullptr) {
                            solePidMatch = nullptr;
                            break;
                        }
                        solePidMatch = candidate;
                    }
                }
                matched = solePidMatch;
            }
            bool created = false;
            if (matched == nullptr) {
                if (obj_pid_new(&matched, critterState.pid) == -1
                    || matched == nullptr
                    || FID_TYPE(matched->fid) != OBJ_TYPE_CRITTER) {
                    std::fprintf(stderr, "Multiplayer snapshot could not create critter pid=%d.\n",
                        critterState.pid);
                    return false;
                }
                matched->flags |= OBJECT_NO_SAVE;
                created = true;
            }
            if (created && obj_attempt_placement(matched, critterState.tile,
                    critterState.elevation, 2) == -1) {
                obj_erase_object(matched, nullptr);
                return false;
            }
            if (session.entities().restoreObject(critterState.entityId, matched) != EntityRegistryError::None) {
                if (created) obj_erase_object(matched, nullptr);
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
                obj_erase_object(stale, nullptr);
            }
        }
        if (!validateCritterState(snapshot)) {
            std::fprintf(stderr, "Multiplayer snapshot critter identity preflight failed.\n");
            return false;
        }
    }
    std::unordered_set<Object*> reboundItems;
    for (const ItemSnapshot& itemState : snapshot.items) {
        Object* desiredHolder = isValid(itemState.holderId)
            ? session.entities().findObject(itemState.holderId)
            : nullptr;
        if (isValid(itemState.holderId) && desiredHolder == nullptr) {
            std::fprintf(stderr, "Multiplayer snapshot item holder missing: item=%u holder=%u.\n",
                itemState.entityId.value, itemState.holderId.value);
            return false;
        }

        Object* matched = nullptr;
        Object* pidFallback = nullptr;
        for (Object* candidate : localItems) {
            if (candidate == nullptr
                || reboundItems.find(candidate) != reboundItems.end()
                || candidate->pid != itemState.itemDescriptor.pid) {
                continue;
            }
            bool locationMatches = desiredHolder != nullptr
                ? candidate->owner == desiredHolder
                : candidate->owner == nullptr
                    && candidate->tile == itemState.tile
                    && candidate->elevation == itemState.elevation;
            if (!locationMatches) {
                continue;
            }
            ItemDescriptor descriptor;
            if (describeItem(candidate, descriptor)
                && itemDescriptorsEqual(descriptor, itemState.itemDescriptor)) {
                matched = candidate;
                break;
            }
            if (pidFallback == nullptr) {
                pidFallback = candidate;
            }
        }
        if (matched == nullptr) {
            matched = pidFallback;
        }
        bool created = false;
        if (matched == nullptr) {
            matched = createItem(itemState.itemDescriptor);
            created = true;
        }
        if (matched == nullptr
            || session.entities().restoreObject(itemState.entityId, matched) != EntityRegistryError::None) {
            std::fprintf(stderr, "Multiplayer snapshot item rebind failed: item=%u pid=%d created=%d.\n",
                itemState.entityId.value, itemState.itemDescriptor.pid, created ? 1 : 0);
            if (created && matched != nullptr) {
                obj_erase_object(matched, nullptr);
            }
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
    for (Object* item : removedItemPointers) {
        if (item != nullptr && removedItemPointers.find(item->owner) == removedItemPointers.end()) {
            obj_destroy(item);
        }
    }

    if (session.applyAuthoritativePhase(snapshot.phase, snapshot.phaseRevision) != LocalSessionError::None
        || !applyActorAndCritterState(snapshot)) {
        std::fprintf(stderr, "Multiplayer snapshot failed phase or actor application.\n");
        return false;
    }
    if (snapshot.phase == SessionPhase::Combat && !snapshot.combat.initiative.empty()) {
        if (!combatTurns.restore(snapshot.combat, combatClockMilliseconds())) {
            return false;
        }
    } else {
        combatTurns.stop();
    }
    combat_free_move = snapshot.combatFreeMove;
    sharedActivity.assign(snapshot.sharedActivity.begin(), snapshot.sharedActivity.end());
    nextSharedActivityId = sharedActivity.empty()
        ? 1 : sharedActivity.back().id + 1;
    if (snapshot.phase == SessionPhase::Dialogue) {
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
    for (const ItemSnapshot& itemState : snapshot.items) {
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
            if (obj_connect(item, itemState.tile, itemState.elevation, nullptr) == -1) {
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
    if (!applyTimedEvents(snapshot)) {
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
    intface_redraw();
    return true;
}

bool networkWorldCaptureAuthoritativeState(EventSequence lastIncludedEvent, WorldSnapshot& snapshot)
{
    return networkWorldCaptureSnapshot(lastIncludedEvent, snapshot);
}

bool networkWorldApplyAuthoritativeState(const WorldSnapshot& snapshot)
{
    return networkWorldApplySnapshot(snapshot);
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

bool networkWorldCombatBeginRound(Object* const* actors, int count)
{
    if (worldMode != NetworkLaunchMode::Host || !session.isActive()
        || session.phase() != SessionPhase::Combat || actors == nullptr || count <= 0
        || count > static_cast<int>(kMaximumCombatInitiative)) {
        return false;
    }
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
        return true;
    }
    return combatTurns.restore(event.state, combatClockMilliseconds());
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

void networkWorldLeave()
{
    combatActionResolving = false;
    combatTurns.stop();
    session.stop();
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
    pendingTalk.reset();
    dialogueCause = {};
    nextDialogueRevision = 1;
    activeLootTargets.clear();
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
    capturedSceneryMapTransition.reset();
    pendingRestProposal.reset();
    expectedSplitEntityId = {};
    lastSplitEntityId = {};
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
