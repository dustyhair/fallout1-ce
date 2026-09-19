#include "multiplayer/network_world.h"

#include <algorithm>
#include <array>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "game/actions.h"
#include "game/anim.h"
#include "game/combat.h"
#include "game/critter.h"
#include "game/intface.h"
#include "game/inventry.h"
#include "game/item.h"
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
std::vector<std::pair<EntityId, Object*>> worldItems;
std::unordered_set<EntityId, EntityIdHash> reservedPickupTargets;
std::unordered_map<Object*, Object*> activeLootTargets;
bool inventoryTransferInProgress = false;

Object* topEnvironmentOrSelf(Object* object)
{
    Object* top = obj_top_environment(object);
    return top != nullptr ? top : object;
}

bool applyInventoryTransfer(Object* source,
    Object* destination,
    Object* item,
    std::uint32_t quantity,
    bool force)
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

    inventoryTransferInProgress = true;
    int rc = force
        ? item_move_force(source, destination, item, static_cast<int>(quantity))
        : item_move(source, destination, item, static_cast<int>(quantity));
    inventoryTransferInProgress = false;
    return rc == 0;
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
        return beginPickup(actor, target)
            ? CommandExecutionStatus::Applied
            : CommandExecutionStatus::InvalidAction;
    }

    CommandExecutionStatus loot(Object* actor, Object* target) override
    {
        if (isInCombat()
            || actor == target
            || target == nullptr
            || actor->elevation != target->elevation
            || FID_TYPE(target->fid) != OBJ_TYPE_CRITTER) {
            return CommandExecutionStatus::InvalidAction;
        }
        activeLootTargets[actor] = target;
        return CommandExecutionStatus::Applied;
    }

    CommandExecutionStatus transferInventory(Object* actor,
        Object* source,
        Object* destination,
        Object* item,
        std::uint32_t quantity) override
    {
        if (actor == nullptr || source == nullptr || destination == nullptr || item == nullptr) {
            return CommandExecutionStatus::InvalidAction;
        }
        auto activeLoot = activeLootTargets.find(actor);
        Object* sourceTop = topEnvironmentOrSelf(source);
        Object* destinationTop = topEnvironmentOrSelf(destination);
        Object* otherTop = sourceTop == actor ? destinationTop : sourceTop;
        if (isInCombat()
            || (sourceTop != actor && destinationTop != actor)
            || activeLoot == activeLootTargets.end()
            || activeLoot->second != otherTop
            || !applyInventoryTransfer(source, destination, item, quantity, false)) {
            return CommandExecutionStatus::InvalidAction;
        }
        return CommandExecutionStatus::Applied;
    }
};

NetworkCommandExecutor commandExecutor;

bool registerWorldObjects()
{
    worldDoors.clear();
    worldItems.clear();
    reservedPickupTargets.clear();
    activeLootTargets.clear();
    std::vector<Object*> doors;
    std::vector<Object*> items;
    std::vector<Object*> critters;
    for (Object* object = obj_find_first(); object != nullptr; object = obj_find_next()) {
        if (obj_is_a_portal(object)) {
            doors.push_back(object);
        } else if (FID_TYPE(object->fid) == OBJ_TYPE_ITEM
            && object->owner == nullptr
            && object->tile >= 0) {
            items.push_back(object);
        } else if (FID_TYPE(object->fid) == OBJ_TYPE_CRITTER
            && object != obj_dude
            && object != peerActor) {
            critters.push_back(object);
        }
    }

    auto stableObjectOrder = [](const Object* lhs, const Object* rhs) {
        return std::tie(lhs->elevation, lhs->tile, lhs->pid, lhs->id, lhs->fid)
            < std::tie(rhs->elevation, rhs->tile, rhs->pid, rhs->id, rhs->fid);
    };
    std::sort(doors.begin(), doors.end(), stableObjectOrder);
    std::sort(items.begin(), items.end(), stableObjectOrder);
    std::sort(critters.begin(), critters.end(), stableObjectOrder);

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

    for (Object* door : doors) {
        EntityRegistrationResult registration = session.registerWorldObject(door);
        if (!registration) {
            return false;
        }
        worldDoors.emplace_back(registration.entityId, door);
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
        if (!session.registerWorldObject(critter)
            || !registerInventory(registerInventory, critter)) {
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
    if (!registerWorldObjects()
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

    ScopedActingPlayerContext actingPlayer(*player, actor);
    return beginPickup(actor, target);
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
        || FID_TYPE(target->fid) != OBJ_TYPE_CRITTER
        || actor->elevation != target->elevation) {
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

bool networkWorldBeginLocalLoot(Object* target)
{
    if (!session.isActive()
        || obj_dude == nullptr
        || target == nullptr
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
    if (source == nullptr || destination == nullptr || item == nullptr) {
        return false;
    }
    if (item->owner == destination) {
        inven_refresh_loot_window();
        return true;
    }
    bool applied = applyInventoryTransfer(source, destination, item, transfer.quantity, true);
    if (applied) {
        inven_refresh_loot_window();
    }
    return applied;
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
    if (!session.isActive() || target == nullptr || succeeded) {
        return;
    }
    std::optional<EntityId> targetId = session.entities().findEntity(target);
    if (targetId.has_value()) {
        reservedPickupTargets.erase(*targetId);
    }
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
    for (const auto& entry : worldItems) {
        Object* item = entry.second;
        if (item == nullptr || session.entities().findObject(entry.first) != item) {
            return false;
        }
        ItemSnapshot itemState;
        itemState.entityId = entry.first;
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
        captured.items.push_back(itemState);
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
    for (const ItemSnapshot& itemState : snapshot.items) {
        Object* item = session.entities().findObject(itemState.entityId);
        Object* holder = isValid(itemState.holderId)
            ? session.entities().findObject(itemState.holderId)
            : nullptr;
        if (item == nullptr || (isValid(itemState.holderId) && holder == nullptr)) {
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
    for (const ItemSnapshot& itemState : snapshot.items) {
        Object* item = session.entities().findObject(itemState.entityId);
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
        } else if (item->tile != itemState.tile || item->elevation != itemState.elevation) {
            Rect dirtyRect;
            if (obj_move_to_tile(item, itemState.tile, itemState.elevation, &dirtyRect) == -1) {
                return false;
            }
            tile_refresh_rect(&dirtyRect, itemState.elevation);
        }
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
    worldItems.clear();
    reservedPickupTargets.clear();
    activeLootTargets.clear();
    erasePeerActor();
}

bool networkWorldInventoryTransferInProgress()
{
    return inventoryTransferInProgress;
}

bool networkWorldActive()
{
    return session.isActive() && peerActor != nullptr;
}

} // namespace multiplayer
} // namespace fallout
