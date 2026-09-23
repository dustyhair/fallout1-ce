#include "multiplayer/network_world.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "game/actions.h"
#include "game/anim.h"
#include "game/combat.h"
#include "game/critter.h"
#include "game/engine_execution_probe.h"
#include "game/game.h"
#include "game/intface.h"
#include "game/inventry.h"
#include "game/item.h"
#include "game/map.h"
#include "game/map_defs.h"
#include "game/object.h"
#include "game/protinst.h"
#include "game/queue.h"
#include "game/scripts.h"
#include "game/stat.h"
#include "game/tile.h"
#include "multiplayer/acting_player_context.h"
#include "multiplayer/local_player_context.h"
#include "multiplayer/local_session.h"
#include "multiplayer/presentation_bridge.h"
#include "plib/gnw/debug.h"

namespace fallout {
namespace multiplayer {
namespace {

LocalSession session;
Object* peerActor = nullptr;
CommandProcessor commandProcessor;
std::vector<std::pair<EntityId, Object*>> worldDoors;
std::vector<std::pair<EntityId, Object*>> worldItems;
std::vector<std::pair<EntityId, Object*>> worldCritters;
std::unordered_set<EntityId, EntityIdHash> reservedPickupTargets;
struct PendingPickup {
    EntityId actorId;
    CommandSequence commandSequence;
};
std::unordered_map<EntityId, PendingPickup, EntityIdHash> pendingPickups;
std::deque<GameEvent> deferredEvents;
std::unordered_map<Object*, Object*> activeLootTargets;
struct ActiveSharedModal {
    EntityId actorId;
    SharedModalKind kind = SharedModalKind::Dialogue;
};
std::optional<ActiveSharedModal> activeSharedModal;
bool inventoryTransferInProgress = false;
bool itemDropInProgress = false;
NetworkLaunchMode worldMode = NetworkLaunchMode::Disabled;
EntityId expectedSplitEntityId;
EntityId lastSplitEntityId;

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

bool validateActorAndCritterState(const WorldSnapshot& snapshot)
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
            return false;
        }
        Rect dirtyRect {};
        bool dirty = false;
        if (actor->tile != actorState.tile || actor->elevation != actorState.elevation) {
            register_clear(actor);
            if (obj_move_to_tile(actor, actorState.tile, actorState.elevation, &dirtyRect) == -1) {
                return false;
            }
            dirty = true;
        }
        if (actor->rotation != actorState.rotation) {
            if (obj_set_rotation(actor, actorState.rotation, &dirtyRect) == -1) {
                return false;
            }
            dirty = true;
        }
        critter_adjust_hits(actor, actorState.hitPoints - critter_get_hits(actor));
        actor->data.critter.combat.ap = actorState.actionPoints;
        actor->data.critter.combat.results = actorState.combatResults;
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
        critter_adjust_hits(critter, critterState.hitPoints - critter_get_hits(critter));
        critter->data.critter.combat.ap = critterState.actionPoints;
        critter->data.critter.combat.results = critterState.combatResults;
        critter->data.critter.combat.team = critterState.team;
        if (dirty) {
            tile_refresh_rect(&dirtyRect, critterState.elevation);
        }
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

    CommandExecutionStatus attack(Object* actor, Object* target, const AttackCommand& command) override
    {
        // Fail closed until the combat controller can prove active-turn
        // ownership and publish complete authoritative effects.
        (void)actor;
        (void)target;
        (void)command;
        return CommandExecutionStatus::InvalidAction;
    }

    SharedModalExecution setSharedModal(Object* actor, const SharedModalCommand& command) override
    {
        SharedModalExecution execution;
        std::optional<EntityId> actorId = session.entities().findEntity(actor);
        if (!actorId.has_value() || !isValid(command.kind)) {
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
    worldDoors.clear();
    worldItems.clear();
    worldCritters.clear();
    reservedPickupTargets.clear();
    pendingPickups.clear();
    deferredEvents.clear();
    activeLootTargets.clear();
    activeSharedModal.reset();
    std::vector<Object*> doors;
    std::vector<Object*> items;
    std::vector<Object*> critters;
    for (Object* object = obj_find_first(); object != nullptr; object = obj_find_next()) {
        if (object == obj_dude || object == peerActor) {
            continue;
        }
        int objectType = FID_TYPE(object->fid);
        if (objectType == OBJ_TYPE_SCENERY && obj_is_a_portal(object)) {
            doors.push_back(object);
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
        EntityRegistrationResult registration = session.registerWorldObject(critter);
        if (!registration || !registerInventory(registerInventory, critter)) {
            return false;
        }
        worldCritters.emplace_back(registration.entityId, critter);
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
    }
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
    GameCommand open;
    open.sequence = CommandSequence { 1 };
    open.playerId = kGuestPlayerId;
    open.actorId = actorId;
    open.expectedPhase = SessionPhase::Exploration;
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
    return openPassed && blockPassed && closePassed;
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

bool networkWorldSynchronizeEnginePhase()
{
    if (!session.isActive()) {
        return false;
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

bool networkWorldSharedModalActive()
{
    return activeSharedModal.has_value()
        || session.phase() == SessionPhase::Dialogue
        || session.phase() == SessionPhase::Transition;
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
    captured.gameTime = game_time();
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
            player->build,
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
    if (!session.isActive()
        || validateSnapshot(snapshot) != SnapshotError::None
        || snapshot.doors.size() != worldDoors.size()
        || !validateVariableState(snapshot)) {
        return false;
    }

    if (!validateActorAndCritterState(snapshot)) {
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
    std::unordered_set<EntityId, EntityIdHash> snapshotItemIds;
    for (const ItemSnapshot& itemState : snapshot.items) {
        snapshotItemIds.insert(itemState.entityId);
    }
    for (const ItemSnapshot& itemState : snapshot.items) {
        if (isValid(itemState.holderId)
            && session.entities().findObject(itemState.holderId) == nullptr
            && snapshotItemIds.find(itemState.holderId) == snapshotItemIds.end()) {
            return false;
        }
    }
    for (const ItemSnapshot& itemState : snapshot.items) {
        if (session.entities().findObject(itemState.entityId) == nullptr) {
            Object* item = createItem(itemState.itemDescriptor);
            if (item == nullptr
                || session.entities().restoreObject(itemState.entityId, item) != EntityRegistryError::None) {
                if (item != nullptr) {
                    obj_erase_object(item, nullptr);
                }
                return false;
            }
            trackWorldItem(itemState.entityId, item);
        }
    }

    if (session.applyAuthoritativePhase(snapshot.phase, snapshot.phaseRevision) != LocalSessionError::None
        || !applyActorAndCritterState(snapshot)) {
        return false;
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
        if (!applyItemDescriptor(item, itemState.itemDescriptor)) {
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
    if (!applyTimedEvents(snapshot)) {
        return false;
    }
    if (activeSharedModal.has_value()
        && sharedModalPhase(activeSharedModal->kind) != snapshot.phase) {
        activeSharedModal.reset();
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
    session.stop();
    worldDoors.clear();
    worldItems.clear();
    worldCritters.clear();
    reservedPickupTargets.clear();
    pendingPickups.clear();
    deferredEvents.clear();
    activeLootTargets.clear();
    activeSharedModal.reset();
    itemDropInProgress = false;
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

    std::array<std::pair<PlayerCharacterState*, Object*>, 2> party;
    std::size_t index = 0;
    for (PlayerId playerId : { kHostPlayerId, kGuestPlayerId }) {
        PlayerCharacterState* player = session.players().find(playerId);
        Object* actor = player != nullptr ? session.entities().findObject(player->actorId) : nullptr;
        if (player == nullptr || actor == nullptr) {
            return PartyExperienceResult::Failed;
        }
        party[index++] = { player, actor };
    }

    for (const auto& member : party) {
        ScopedActingPlayerContext actingPlayer(*member.first, member.second);
        if (stat_pc_add_experience(xp) != 0) {
            return PartyExperienceResult::Failed;
        }
    }
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

} // namespace multiplayer
} // namespace fallout
