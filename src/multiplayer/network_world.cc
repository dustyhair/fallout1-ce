#include "multiplayer/network_world.h"

#include <algorithm>
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

bool registerWorldDoors()
{
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
        if (!session.registerWorldObject(door)) {
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
    return true;
}

bool networkWorldApplyPeerMove(const ActorMovementStartedEvent& movement)
{
    if (!session.isActive() || peerActor == nullptr) {
        return false;
    }

    PlayerCharacterState* remotePlayer = session.players().findByActor(movement.actorId);
    if (remotePlayer == nullptr
        || remotePlayer->ownership != PlayerOwnership::RemoteControl
        || session.entities().findObject(remotePlayer->actorId) != peerActor
        || !hexGridTileIsValid(movement.destinationTile)
        || movement.elevation != peerActor->elevation) {
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

    register_clear(peerActor);
    if (!movement.path.empty() && peerActor->tile != movement.startingTile) {
        Rect dirtyRect;
        if (obj_move_to_tile(peerActor, movement.startingTile, movement.elevation, &dirtyRect) == -1) {
            return false;
        }
        tile_refresh_rect(&dirtyRect, peerActor->elevation);
    }
    if (register_begin(ANIMATION_REQUEST_UNRESERVED) == -1) {
        return false;
    }
    int rc;
    if (!movement.path.empty()) {
        rc = register_object_move_along_path(peerActor,
            movement.destinationTile,
            movement.elevation,
            movement.path.data(),
            static_cast<int>(movement.path.size()),
            movement.running,
            0);
    } else {
        rc = movement.running
            ? register_object_run_to_tile(peerActor, movement.destinationTile, movement.elevation, -1, 0)
            : register_object_move_to_tile(peerActor, movement.destinationTile, movement.elevation, -1, 0);
    }
    int endRc = register_end();
    return rc != -1 && endRc != -1;
}

bool networkWorldApplyPeerFacing(const ActorFacingChangedEvent& facing)
{
    if (!session.isActive() || peerActor == nullptr) {
        return false;
    }

    PlayerCharacterState* remotePlayer = session.players().findByActor(facing.actorId);
    if (remotePlayer == nullptr
        || remotePlayer->ownership != PlayerOwnership::RemoteControl
        || session.entities().findObject(remotePlayer->actorId) != peerActor
        || facing.rotation < 0
        || facing.rotation >= ROTATION_COUNT) {
        return false;
    }

    Rect dirtyRect;
    if (obj_set_rotation(peerActor, facing.rotation, &dirtyRect) == -1) {
        return false;
    }
    tile_refresh_rect(&dirtyRect, peerActor->elevation);
    return true;
}

bool networkWorldApplyPeerDoorUse(const DoorUseStartedEvent& doorUse)
{
    if (!session.isActive() || peerActor == nullptr || isInCombat()) {
        return false;
    }

    PlayerCharacterState* remotePlayer = session.players().findByActor(doorUse.actorId);
    Object* target = session.entities().findObject(doorUse.targetId);
    if (remotePlayer == nullptr
        || remotePlayer->ownership != PlayerOwnership::RemoteControl
        || session.entities().findObject(remotePlayer->actorId) != peerActor
        || target == nullptr
        || !obj_is_a_portal(target)
        || peerActor->elevation != target->elevation) {
        return false;
    }

    ScopedActingPlayerContext actingPlayer(*remotePlayer, peerActor);
    return action_use_an_object(peerActor, target) != -1;
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
    erasePeerActor();
}

bool networkWorldActive()
{
    return session.isActive() && peerActor != nullptr;
}

} // namespace multiplayer
} // namespace fallout
