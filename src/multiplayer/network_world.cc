#include "multiplayer/network_world.h"

#include <algorithm>
#include <array>
#include <tuple>
#include <vector>

#include "game/actions.h"
#include "game/anim.h"
#include "game/combat.h"
#include "game/critter.h"
#include "game/intface.h"
#include "game/map_defs.h"
#include "game/object.h"
#include "game/protinst.h"
#include "game/stat.h"
#include "game/tile.h"
#include "multiplayer/acting_player_context.h"
#include "multiplayer/local_player_context.h"
#include "multiplayer/local_session.h"
#include "multiplayer/presentation_bridge.h"

namespace fallout {
namespace multiplayer {
namespace {

LocalSession session;
Object* peerActor = nullptr;
CommandProcessor commandProcessor;
std::vector<std::pair<EntityId, Object*>> worldDoors;

class NetworkCommandExecutor : public CommandExecutor {
public:
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

    CommandExecutionStatus useDoor(Object* actor, Object* target) override
    {
        if (isInCombat()
            || target == nullptr
            || actor->elevation != target->elevation
            || !obj_is_a_portal(target)
            || action_use_an_object(actor, target) == -1) {
            return CommandExecutionStatus::InvalidAction;
        }
        return CommandExecutionStatus::Applied;
    }

    CommandExecutionStatus pickup(Object* actor, Object* target) override
    {
        if (isInCombat()
            || actor == target
            || target == nullptr
            || actor->elevation != target->elevation
            || FID_TYPE(target->fid) != OBJ_TYPE_ITEM
            || target->owner != nullptr
            || action_get_an_object(actor, target) == -1) {
            return CommandExecutionStatus::InvalidAction;
        }
        return CommandExecutionStatus::Applied;
    }

    CommandExecutionStatus loot(Object* actor, Object* target) override
    {
        if (isInCombat()
            || actor == target
            || target == nullptr
            || actor->elevation != target->elevation
            || FID_TYPE(target->fid) != OBJ_TYPE_CRITTER
            || action_loot_container(actor, target) == -1) {
            return CommandExecutionStatus::InvalidAction;
        }
        return CommandExecutionStatus::Applied;
    }
};

NetworkCommandExecutor commandExecutor;

bool registerWorldDoors()
{
    worldDoors.clear();
    std::vector<Object*> doors;
    for (Object* object = obj_find_first(); object != nullptr; object = obj_find_next()) {
        if (obj_is_a_portal(object)) {
            doors.push_back(object);
        }
    }

    std::sort(doors.begin(), doors.end(), [](const Object* lhs, const Object* rhs) {
        return std::tie(lhs->elevation, lhs->tile, lhs->pid, lhs->id, lhs->fid)
            < std::tie(rhs->elevation, rhs->tile, rhs->pid, rhs->id, rhs->fid);
    });

    for (Object* door : doors) {
        EntityRegistrationResult registration = session.registerWorldObject(door);
        if (!registration) {
            return false;
        }
        worldDoors.emplace_back(registration.entityId, door);
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
    if (!registerWorldDoors()
        || !refreshPlayer(kHostPlayerId)
        || !refreshPlayer(kGuestPlayerId)) {
        session.stop();
        erasePeerActor();
        return false;
    }

    intface_redraw();
    commandProcessor.reset();
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

    ScopedActingPlayerContext actingPlayer(*player, actor);
    return action_use_an_object(actor, target) != -1;
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
    if (result.event.has_value()) {
        if (auto* movement = std::get_if<ActorMovementStartedEvent>(&result.event->payload)) {
            movement->startingTile = startingTile;
            movement->path = std::move(path);
        }
    }
    return result;
}

SessionPhase networkWorldPhase()
{
    return session.phase();
}

std::uint32_t networkWorldPhaseRevision()
{
    return session.phaseRevision();
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
    for (PlayerId playerId : { kHostPlayerId, kGuestPlayerId }) {
        EntityId actorId = session.playerActorId(playerId);
        Object* actor = session.entities().findObject(actorId);
        if (actor == nullptr || anim_busy(actor) == -1) {
            return false;
        }
        captured.actors.push_back(ActorSnapshot {
            actorId,
            playerId,
            actor->tile,
            actor->elevation,
            actor->rotation,
            critter_get_hits(actor),
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
    if (validateSnapshot(captured) != SnapshotError::None) {
        return false;
    }
    snapshot = std::move(captured);
    return true;
}

bool networkWorldApplySnapshot(const WorldSnapshot& snapshot)
{
    if (!session.isActive()
        || validateSnapshot(snapshot) != SnapshotError::None
        || snapshot.phase != session.phase()
        || snapshot.phaseRevision != session.phaseRevision()
        || snapshot.actors.size() != 2
        || snapshot.doors.size() != worldDoors.size()) {
        return false;
    }

    for (const ActorSnapshot& actorState : snapshot.actors) {
        Object* actor = session.entities().findObject(actorState.entityId);
        std::optional<PlayerId> owner = session.entities().ownerOf(actorState.entityId);
        if (actor == nullptr || !owner.has_value() || *owner != actorState.ownerId) {
            return false;
        }
    }
    for (const DoorSnapshot& doorState : snapshot.doors) {
        Object* door = session.entities().findObject(doorState.entityId);
        if (door == nullptr
            || !obj_is_a_portal(door)
            || doorState.open != (doorState.frame != 0)) {
            return false;
        }
    }

    for (const ActorSnapshot& actorState : snapshot.actors) {
        Object* actor = session.entities().findObject(actorState.entityId);
        register_clear(actor);
        Rect dirtyRect;
        if (obj_move_to_tile(actor, actorState.tile, actorState.elevation, &dirtyRect) == -1
            || obj_set_rotation(actor, actorState.rotation, &dirtyRect) == -1) {
            return false;
        }
        int currentHitPoints = critter_get_hits(actor);
        critter_adjust_hits(actor, actorState.hitPoints - currentHitPoints);
        tile_refresh_rect(&dirtyRect, actorState.elevation);
    }
    for (const DoorSnapshot& doorState : snapshot.doors) {
        Object* door = session.entities().findObject(doorState.entityId);
        Rect dirtyRect;
        if (obj_set_frame(door, doorState.frame, &dirtyRect) == -1
            || (doorState.locked ? obj_lock(door) : obj_unlock(door)) == -1) {
            return false;
        }
        tile_refresh_rect(&dirtyRect, door->elevation);
    }
    intface_redraw();
    return true;
}

std::optional<EntityId> networkWorldFindEntity(const Object* object)
{
    if (!session.isActive() || object == nullptr) {
        return std::nullopt;
    }
    return session.entities().findEntity(object);
}

void networkWorldLeave()
{
    session.stop();
    worldDoors.clear();
    erasePeerActor();
}

bool networkWorldActive()
{
    return session.isActive() && peerActor != nullptr;
}

} // namespace multiplayer
} // namespace fallout
