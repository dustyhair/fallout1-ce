#include "multiplayer/gameplay_wire.h"

#include <limits>
#include <variant>

namespace fallout {
namespace multiplayer {
namespace {

enum class CommandType : std::uint8_t {
    Move = 1,
    UseDoor = 2,
    Pickup = 3,
    Loot = 4,
    Face = 5,
    InventoryTransfer = 6,
    ItemDrop = 7,
    Attack = 8,
    SharedModal = 9,
    UseSkill = 10,
    UseItemOn = 11,
    Elevator = 12,
    ExitGrid = 13,
    SceneryTransition = 14,
    Rest = 15,
};

enum class EventType : std::uint8_t {
    ActorMovementStarted = 1,
    DoorUseStarted = 2,
    ItemPickupStarted = 3,
    LootStarted = 4,
    ActorFacingChanged = 5,
    InventoryTransferred = 6,
    ItemDropped = 7,
    AttackStarted = 8,
    ItemPickupCompleted = 9,
    SharedModalStateChanged = 10,
    SkillUseStarted = 11,
    ItemUseStarted = 12,
    ElevatorTransitioned = 13,
    ExitGridTransitioned = 14,
    SceneryTransitioned = 15,
    RestStateChanged = 16,
};

constexpr std::size_t kCommandHeaderSize = 28;
constexpr std::size_t kMoveCommandSize = kCommandHeaderSize + 12;
constexpr std::size_t kTargetCommandSize = kCommandHeaderSize + 4;
constexpr std::size_t kFacingCommandSize = kCommandHeaderSize + 8;
constexpr std::size_t kItemDescriptorSize = 16;
constexpr std::size_t kInventoryTransferCommandSize = kCommandHeaderSize + 20 + kItemDescriptorSize;
constexpr std::size_t kItemDropCommandSize = kCommandHeaderSize + 16 + kItemDescriptorSize;
constexpr std::size_t kAttackCommandSize = kCommandHeaderSize + 12;
constexpr std::size_t kSharedModalCommandSize = kCommandHeaderSize + 4;
constexpr std::size_t kSkillCommandSize = kCommandHeaderSize + 8;
constexpr std::size_t kItemUseCommandSize = kCommandHeaderSize + 8;
constexpr std::size_t kElevatorCommandSize = kCommandHeaderSize + 8;
constexpr std::size_t kCommandResultSize = 24;
constexpr std::size_t kEventHeaderSize = 20;
constexpr std::size_t kMovementEventHeaderSize = kEventHeaderSize + 20;
constexpr std::size_t kTargetEventSize = kEventHeaderSize + 8;
constexpr std::size_t kDoorStateEventSize = kEventHeaderSize + 16;
constexpr std::size_t kPickupCompletedEventSize = kEventHeaderSize + 16 + kItemDescriptorSize;
constexpr std::size_t kFacingEventSize = kEventHeaderSize + 8;
constexpr std::size_t kInventoryTransferEventSize = kEventHeaderSize + 28 + kItemDescriptorSize;
constexpr std::size_t kItemDropEventSize = kEventHeaderSize + 32 + kItemDescriptorSize;
constexpr std::size_t kAttackEventSize = kEventHeaderSize + 16;
constexpr std::size_t kSharedModalEventSize = kEventHeaderSize + 12;
constexpr std::size_t kSkillEventSize = kEventHeaderSize + 12;
constexpr std::size_t kItemUseEventSize = kEventHeaderSize + 12;
constexpr std::size_t kElevatorEventSize = kEventHeaderSize + 40;
constexpr std::size_t kExitGridEventBaseSize = kEventHeaderSize + 20;
constexpr std::size_t kRestEventSize = kEventHeaderSize + 24;
constexpr std::size_t kTransitionPlacementSize = 20;
constexpr std::int32_t kAttackHitModeCount = 20;
constexpr std::int32_t kAttackHitLocationCount = 9;

void appendUInt16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void appendUInt32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void appendUInt64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

void appendInt32(std::vector<std::uint8_t>& bytes, std::int32_t value)
{
    appendUInt32(bytes, static_cast<std::uint32_t>(value));
}

std::uint16_t readUInt16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8)
        | static_cast<std::uint16_t>(bytes[offset + 1]));
}

std::uint32_t readUInt32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24)
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8)
        | static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint64_t readUInt64(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    std::uint64_t value = 0;
    for (int index = 0; index < 8; index++) {
        value = (value << 8) | bytes[offset + index];
    }
    return value;
}

std::int32_t readInt32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::int32_t>(readUInt32(bytes, offset));
}

bool isValidItemDescriptor(const ItemDescriptor& descriptor)
{
    if (!hasItemDescriptor(descriptor)) {
        return descriptor.extendedFlags == 0
            && descriptor.data0 == 0
            && descriptor.data1 == 0;
    }
    return descriptor.pid >= 0
        && (static_cast<std::uint32_t>(descriptor.pid) >> 24) == 0;
}

void appendItemDescriptor(std::vector<std::uint8_t>& bytes, const ItemDescriptor& descriptor)
{
    appendInt32(bytes, descriptor.pid);
    appendInt32(bytes, descriptor.extendedFlags);
    appendInt32(bytes, descriptor.data0);
    appendInt32(bytes, descriptor.data1);
}

ItemDescriptor readItemDescriptor(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return ItemDescriptor {
        readInt32(bytes, offset),
        readInt32(bytes, offset + 4),
        readInt32(bytes, offset + 8),
        readInt32(bytes, offset + 12),
    };
}

GameplayWireError validateEnvelope(const ProtocolEnvelope& envelope, MessageKind expectedKind)
{
    if (envelope.kind != expectedKind) {
        return GameplayWireError::WrongMessageKind;
    }
    if (envelope.sessionId.value == 0) {
        return GameplayWireError::InvalidSessionId;
    }
    if (envelope.sequence == 0) {
        return GameplayWireError::InvalidEnvelopeSequence;
    }
    return GameplayWireError::None;
}

GameplayWireError validateEnvelopeIdentity(const ProtocolEnvelope& envelope)
{
    if (envelope.sessionId.value == 0) {
        return GameplayWireError::InvalidSessionId;
    }
    if (envelope.sequence == 0) {
        return GameplayWireError::InvalidEnvelopeSequence;
    }
    return GameplayWireError::None;
}

bool isKnownPhase(SessionPhase phase)
{
    switch (phase) {
    case SessionPhase::Lobby:
    case SessionPhase::Loading:
    case SessionPhase::Exploration:
    case SessionPhase::Combat:
    case SessionPhase::Dialogue:
    case SessionPhase::Transition:
    case SessionPhase::Ending:
        return true;
    }
    return false;
}

bool isKnownRejection(CommandRejection rejection)
{
    switch (rejection) {
    case CommandRejection::Malformed:
    case CommandRejection::WrongPhase:
    case CommandRejection::Stale:
    case CommandRejection::NotOwner:
    case CommandRejection::MissingEntity:
    case CommandRejection::InvalidAction:
        return true;
    case CommandRejection::None:
        return false;
    }
    return false;
}

GameplayWireError validateCommand(const GameCommand& command)
{
    if (command.sequence.value == 0) {
        return GameplayWireError::InvalidCommandSequence;
    }
    if (command.playerId != kHostPlayerId && command.playerId != kGuestPlayerId) {
        return GameplayWireError::InvalidPlayerId;
    }
    if (!isValid(command.actorId)) {
        return GameplayWireError::InvalidEntityId;
    }
    if (!isKnownPhase(command.expectedPhase)) {
        return GameplayWireError::InvalidPhase;
    }
    if (command.expectedPhaseRevision == 0) {
        return GameplayWireError::InvalidPhaseRevision;
    }
    if (const auto* move = std::get_if<MoveCommand>(&command.payload)) {
        if (move->destinationTile < 0 || move->elevation < 0 || move->elevation > 2) {
            return GameplayWireError::InvalidMove;
        }
        return GameplayWireError::None;
    }
    if (const auto* face = std::get_if<FaceCommand>(&command.payload)) {
        return face->rotation >= 0 && face->rotation < kActorRotationCount
            ? GameplayWireError::None
            : GameplayWireError::InvalidRotation;
    }
    if (const auto* transfer = std::get_if<InventoryTransferCommand>(&command.payload)) {
        if (!isValid(transfer->sourceId)
            || !isValid(transfer->destinationId)
            || transfer->sourceId == transfer->destinationId) {
            return GameplayWireError::InvalidEntityId;
        }
        if (!isValidItemDescriptor(transfer->itemDescriptor)
            || (!isValid(transfer->itemId) && !hasItemDescriptor(transfer->itemDescriptor))) {
            return GameplayWireError::InvalidEntityId;
        }
        return transfer->quantity != 0
                && transfer->quantity <= transfer->sourceQuantity
                && transfer->sourceQuantity <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
                && transfer->quantity <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
            ? GameplayWireError::None
            : GameplayWireError::InvalidQuantity;
    }
    if (const auto* drop = std::get_if<ItemDropCommand>(&command.payload)) {
        if (!isValid(drop->sourceId)
            || !isValidItemDescriptor(drop->itemDescriptor)
            || (!isValid(drop->itemId) && !hasItemDescriptor(drop->itemDescriptor))) {
            return GameplayWireError::InvalidEntityId;
        }
        return drop->quantity != 0
                && drop->quantity <= drop->sourceQuantity
                && drop->sourceQuantity <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
            ? GameplayWireError::None
            : GameplayWireError::InvalidQuantity;
    }
    if (const auto* attack = std::get_if<AttackCommand>(&command.payload)) {
        if (!isValid(attack->targetId)) {
            return GameplayWireError::InvalidEntityId;
        }
        return attack->hitMode >= 0 && attack->hitMode < kAttackHitModeCount
                && attack->hitLocation >= 0 && attack->hitLocation < kAttackHitLocationCount
            ? GameplayWireError::None
            : GameplayWireError::InvalidAttack;
    }
    if (const auto* modal = std::get_if<SharedModalCommand>(&command.payload)) {
        return isValid(modal->kind)
            ? GameplayWireError::None
            : GameplayWireError::InvalidModal;
    }
    if (const auto* skill = std::get_if<UseSkillCommand>(&command.payload)) {
        if (!isValid(skill->targetId)) {
            return GameplayWireError::InvalidEntityId;
        }
        return isValid(skill->skill)
            ? GameplayWireError::None
            : GameplayWireError::InvalidSkill;
    }
    if (const auto* itemUse = std::get_if<UseItemOnCommand>(&command.payload)) {
        return isValid(itemUse->itemId)
                && isValid(itemUse->targetId)
                && itemUse->itemId != itemUse->targetId
            ? GameplayWireError::None
            : GameplayWireError::InvalidEntityId;
    }
    if (const auto* elevator = std::get_if<ElevatorCommand>(&command.payload)) {
        return elevator->elevatorType >= 0
                && elevator->elevatorType < 12
                && elevator->destinationLevel >= 0
                && elevator->destinationLevel < 4
            ? GameplayWireError::None
            : GameplayWireError::InvalidMove;
    }
    if (const auto* exitGrid = std::get_if<ExitGridCommand>(&command.payload)) {
        return isValid(exitGrid->exitId)
            ? GameplayWireError::None
            : GameplayWireError::InvalidEntityId;
    }
    if (const auto* transition = std::get_if<SceneryTransitionCommand>(&command.payload)) {
        return isValid(transition->transitionId)
            ? GameplayWireError::None
            : GameplayWireError::InvalidEntityId;
    }
    if (const auto* rest = std::get_if<RestCommand>(&command.payload)) {
        return isValidRestMinutes(rest->minutes)
            ? GameplayWireError::None
            : GameplayWireError::InvalidMove;
    }

    EntityId targetId;
    if (const auto* interact = std::get_if<InteractCommand>(&command.payload)) {
        targetId = interact->targetId;
    } else if (const auto* pickup = std::get_if<PickupCommand>(&command.payload)) {
        targetId = pickup->targetId;
    } else if (const auto* loot = std::get_if<LootCommand>(&command.payload)) {
        targetId = loot->targetId;
    }
    return isValid(targetId) ? GameplayWireError::None : GameplayWireError::InvalidEntityId;
}

GameplayWireError validateResult(const CommandResult& result)
{
    if (result.commandSequence.value == 0) {
        return GameplayWireError::InvalidCommandSequence;
    }
    if (result.status == CommandStatus::Accepted) {
        if (result.rejection != CommandRejection::None
            || result.firstEventSequence.value == 0
            || result.eventCount == 0) {
            return GameplayWireError::InconsistentResult;
        }
        return GameplayWireError::None;
    }
    if (result.status == CommandStatus::Rejected) {
        if (!isKnownRejection(result.rejection)) {
            return GameplayWireError::InvalidRejection;
        }
        if (result.firstEventSequence.value != 0
            || result.eventCount != 0) {
            return GameplayWireError::InconsistentResult;
        }
        return GameplayWireError::None;
    }
    return GameplayWireError::InvalidStatus;
}

GameplayWireError validateEvent(const GameEvent& event)
{
    if (event.sequence.value == 0) {
        return GameplayWireError::InvalidEventSequence;
    }
    if (event.causedBy.value == 0) {
        return GameplayWireError::InvalidCommandSequence;
    }
    if (const auto* movement = std::get_if<ActorMovementStartedEvent>(&event.payload)) {
        if (!isValid(movement->actorId)) {
            return GameplayWireError::InvalidEntityId;
        }
        if (movement->destinationTile < 0 || movement->elevation < 0 || movement->elevation > 2) {
            return GameplayWireError::InvalidMove;
        }
        if (movement->path.size() > kMaximumMovementPathLength
            || (movement->path.empty() && movement->startingTile != -1)
            || (!movement->path.empty() && movement->startingTile < 0)) {
            return GameplayWireError::InvalidMove;
        }
        for (std::uint8_t rotation : movement->path) {
            if (rotation >= kActorRotationCount) {
                return GameplayWireError::InvalidMove;
            }
        }
        return GameplayWireError::None;
    }
    if (const auto* facing = std::get_if<ActorFacingChangedEvent>(&event.payload)) {
        if (!isValid(facing->actorId)) {
            return GameplayWireError::InvalidEntityId;
        }
        if (facing->rotation < 0 || facing->rotation >= kActorRotationCount) {
            return GameplayWireError::InvalidRotation;
        }
        return GameplayWireError::None;
    }
    if (const auto* transfer = std::get_if<InventoryTransferredEvent>(&event.payload)) {
        if (!isValid(transfer->actorId)
            || !isValid(transfer->sourceId)
            || !isValid(transfer->destinationId)
            || !isValid(transfer->itemId)
            || (isValid(transfer->remainderItemId)
                && (transfer->remainderItemId == transfer->itemId
                    || transfer->remainderItemId == transfer->sourceId
                    || transfer->remainderItemId == transfer->destinationId))
            || !isValidItemDescriptor(transfer->itemDescriptor)
            || !hasItemDescriptor(transfer->itemDescriptor)
            || ((transfer->quantity < transfer->sourceQuantity) != isValid(transfer->remainderItemId))
            || transfer->sourceId == transfer->destinationId) {
            return GameplayWireError::InvalidEntityId;
        }
        return transfer->quantity != 0
                && transfer->quantity <= transfer->sourceQuantity
                && transfer->sourceQuantity <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
                && transfer->quantity <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
            ? GameplayWireError::None
            : GameplayWireError::InvalidQuantity;
    }
    if (const auto* drop = std::get_if<ItemDroppedEvent>(&event.payload)) {
        if (!isValid(drop->actorId)
            || !isValid(drop->sourceId)
            || !isValid(drop->itemId)
            || (isValid(drop->remainderItemId)
                && (drop->remainderItemId == drop->actorId
                    || drop->remainderItemId == drop->sourceId
                    || drop->remainderItemId == drop->itemId))
            || !isValidItemDescriptor(drop->itemDescriptor)
            || !hasItemDescriptor(drop->itemDescriptor)
            || ((drop->quantity < drop->sourceQuantity) != isValid(drop->remainderItemId))) {
            return GameplayWireError::InvalidEntityId;
        }
        if (drop->tile < 0 || drop->elevation < 0 || drop->elevation > 2) {
            return GameplayWireError::InvalidMove;
        }
        return drop->quantity != 0
                && drop->quantity <= drop->sourceQuantity
                && drop->sourceQuantity <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
            ? GameplayWireError::None
            : GameplayWireError::InvalidQuantity;
    }
    if (const auto* attack = std::get_if<AttackStartedEvent>(&event.payload)) {
        if (!isValid(attack->actorId) || !isValid(attack->targetId)) {
            return GameplayWireError::InvalidEntityId;
        }
        return attack->hitMode >= 0 && attack->hitMode < kAttackHitModeCount
                && attack->hitLocation >= 0 && attack->hitLocation < kAttackHitLocationCount
            ? GameplayWireError::None
            : GameplayWireError::InvalidAttack;
    }
    if (const auto* pickup = std::get_if<ItemPickupCompletedEvent>(&event.payload)) {
        if (!isValid(pickup->actorId)
            || !isValid(pickup->targetId)
            || !isValidItemDescriptor(pickup->itemDescriptor)) {
            return GameplayWireError::InvalidEntityId;
        }
        if (pickup->succeeded) {
            return pickup->quantity != 0
                    && pickup->quantity <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
                    && hasItemDescriptor(pickup->itemDescriptor)
                ? GameplayWireError::None
                : GameplayWireError::InvalidQuantity;
        }
        return pickup->quantity == 0 && !hasItemDescriptor(pickup->itemDescriptor)
            ? GameplayWireError::None
            : GameplayWireError::InvalidQuantity;
    }
    if (const auto* modal = std::get_if<SharedModalStateChangedEvent>(&event.payload)) {
        if (!isValid(modal->actorId)
            || !isValid(modal->kind)
            || modal->phaseRevision == 0) {
            return GameplayWireError::InvalidModal;
        }
        SessionPhase expectedPhase = modal->open
            ? sharedModalPhase(modal->kind)
            : SessionPhase::Exploration;
        return modal->phase == expectedPhase
            ? GameplayWireError::None
            : GameplayWireError::InvalidModal;
    }
    if (const auto* skill = std::get_if<SkillUseStartedEvent>(&event.payload)) {
        if (!isValid(skill->actorId) || !isValid(skill->targetId)) {
            return GameplayWireError::InvalidEntityId;
        }
        return isValid(skill->skill)
            ? GameplayWireError::None
            : GameplayWireError::InvalidSkill;
    }
    if (const auto* itemUse = std::get_if<ItemUseStartedEvent>(&event.payload)) {
        return isValid(itemUse->actorId)
                && isValid(itemUse->itemId)
                && isValid(itemUse->targetId)
                && itemUse->actorId != itemUse->itemId
                && itemUse->itemId != itemUse->targetId
            ? GameplayWireError::None
            : GameplayWireError::InvalidEntityId;
    }
    if (const auto* elevator = std::get_if<ElevatorTransitionedEvent>(&event.payload)) {
        return isValid(elevator->actorId)
                && elevator->elevatorType >= 0
                && elevator->elevatorType < 12
                && elevator->map >= 0
                && elevator->hostTile >= 0
                && elevator->hostElevation >= 0
                && elevator->hostElevation <= 2
                && elevator->hostRotation >= 0
                && elevator->hostRotation < kActorRotationCount
                && elevator->guestTile >= 0
                && elevator->guestElevation >= 0
                && elevator->guestElevation <= 2
                && elevator->guestRotation >= 0
                && elevator->guestRotation < kActorRotationCount
                && (elevator->hostElevation != elevator->guestElevation
                    || elevator->hostTile != elevator->guestTile)
                && elevator->phaseRevision != 0
            ? GameplayWireError::None
            : GameplayWireError::InvalidMove;
    }
    if (const auto* exitGrid = std::get_if<ExitGridTransitionedEvent>(&event.payload)) {
        if (!isValid(exitGrid->actorId)
            || !isValid(exitGrid->exitId)
            || exitGrid->map < 0
            || exitGrid->phaseRevision == 0
            || exitGrid->placements.empty()
            || exitGrid->placements.size() > kMaximumTransitionPlayers) {
            return GameplayWireError::InvalidMove;
        }
        bool actingPlayerPresent = false;
        for (std::size_t index = 0; index < exitGrid->placements.size(); index++) {
            const PlayerTransitionPlacement& placement = exitGrid->placements[index];
            if (!isValid(placement.playerId)
                || !isValid(placement.actorId)
                || placement.tile < 0
                || placement.elevation < 0
                || placement.elevation > 2
                || placement.rotation < 0
                || placement.rotation >= kActorRotationCount) {
                return GameplayWireError::InvalidMove;
            }
            actingPlayerPresent = actingPlayerPresent || placement.actorId == exitGrid->actorId;
            for (std::size_t previous = 0; previous < index; previous++) {
                if (exitGrid->placements[previous].playerId == placement.playerId
                    || exitGrid->placements[previous].actorId == placement.actorId) {
                    return GameplayWireError::InvalidMove;
                }
            }
        }
        return actingPlayerPresent
            ? GameplayWireError::None
            : GameplayWireError::InvalidMove;
    }
    if (const auto* transition = std::get_if<SceneryTransitionedEvent>(&event.payload)) {
        if (!isValid(transition->actorId)
            || !isValid(transition->transitionId)
            || transition->map < 0
            || transition->phaseRevision == 0
            || transition->placements.empty()
            || transition->placements.size() > kMaximumTransitionPlayers) {
            return GameplayWireError::InvalidMove;
        }
        bool actingPlayerPresent = false;
        for (std::size_t index = 0; index < transition->placements.size(); index++) {
            const PlayerTransitionPlacement& placement = transition->placements[index];
            if (!isValid(placement.playerId)
                || !isValid(placement.actorId)
                || placement.tile < 0
                || placement.elevation < 0
                || placement.elevation > 2
                || placement.rotation < 0
                || placement.rotation >= kActorRotationCount) {
                return GameplayWireError::InvalidMove;
            }
            actingPlayerPresent = actingPlayerPresent || placement.actorId == transition->actorId;
            for (std::size_t previous = 0; previous < index; previous++) {
                if (transition->placements[previous].playerId == placement.playerId
                    || transition->placements[previous].actorId == placement.actorId) {
                    return GameplayWireError::InvalidMove;
                }
            }
        }
        return actingPlayerPresent
            ? GameplayWireError::None
            : GameplayWireError::InvalidMove;
    }
    if (const auto* rest = std::get_if<RestStateChangedEvent>(&event.payload)) {
        return isValid(rest->actorId)
                && isValidRestMinutes(rest->minutes)
                && (!rest->completed || rest->minutes != 0)
                && (!rest->interrupted || rest->completed)
                && rest->gameTime > 0
                && rest->phaseRevision > 0
            ? GameplayWireError::None
            : GameplayWireError::InvalidMove;
    }

    EntityId actorId;
    EntityId targetId;
    if (const auto* door = std::get_if<DoorUseStartedEvent>(&event.payload)) {
        if (!isValid(door->actorId)
            || !isValid(door->targetId)
            || door->frame < 0
            || door->open != (door->frame != 0)) {
            return GameplayWireError::InvalidMove;
        }
        return GameplayWireError::None;
    } else if (const auto* pickup = std::get_if<ItemPickupStartedEvent>(&event.payload)) {
        actorId = pickup->actorId;
        targetId = pickup->targetId;
    } else if (const auto* loot = std::get_if<LootStartedEvent>(&event.payload)) {
        actorId = loot->actorId;
        targetId = loot->targetId;
    }
    return isValid(actorId) && isValid(targetId) ? GameplayWireError::None : GameplayWireError::InvalidEntityId;
}

void appendCommandHeader(const GameCommand& command, CommandType type, std::vector<std::uint8_t>& payload)
{
    appendUInt16(payload, kGameplayWireVersion);
    payload.push_back(static_cast<std::uint8_t>(type));
    payload.push_back(0);
    appendUInt64(payload, command.sequence.value);
    appendUInt32(payload, command.playerId.value);
    appendUInt32(payload, command.actorId.value);
    payload.push_back(static_cast<std::uint8_t>(command.expectedPhase));
    payload.insert(payload.end(), 3, 0);
    appendUInt32(payload, command.expectedPhaseRevision);
}

void appendEventHeader(const GameEvent& event, EventType type, std::vector<std::uint8_t>& payload)
{
    appendUInt16(payload, kGameplayWireVersion);
    payload.push_back(static_cast<std::uint8_t>(type));
    payload.push_back(0);
    appendUInt64(payload, event.sequence.value);
    appendUInt64(payload, event.causedBy.value);
}

} // namespace

GameplayWireError encodeGameCommand(const GameCommand& command, ProtocolEnvelope& envelope)
{
    GameplayWireError error = validateEnvelopeIdentity(envelope);
    if (error != GameplayWireError::None) {
        return error;
    }
    error = validateCommand(command);
    if (error != GameplayWireError::None) {
        return error;
    }

    envelope.kind = MessageKind::Command;
    envelope.payload.clear();
    if (const auto* move = std::get_if<MoveCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::Move, envelope.payload);
        appendInt32(envelope.payload, move->destinationTile);
        appendInt32(envelope.payload, move->elevation);
        envelope.payload.push_back(move->running ? 1 : 0);
        envelope.payload.insert(envelope.payload.end(), 3, 0);
    } else if (const auto* face = std::get_if<FaceCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::Face, envelope.payload);
        appendInt32(envelope.payload, face->rotation);
        appendUInt32(envelope.payload, 0);
    } else if (const auto* interact = std::get_if<InteractCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::UseDoor, envelope.payload);
        appendUInt32(envelope.payload, interact->targetId.value);
    } else if (const auto* pickup = std::get_if<PickupCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::Pickup, envelope.payload);
        appendUInt32(envelope.payload, pickup->targetId.value);
    } else if (const auto* loot = std::get_if<LootCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::Loot, envelope.payload);
        appendUInt32(envelope.payload, loot->targetId.value);
    } else if (const auto* transfer = std::get_if<InventoryTransferCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::InventoryTransfer, envelope.payload);
        appendUInt32(envelope.payload, transfer->sourceId.value);
        appendUInt32(envelope.payload, transfer->destinationId.value);
        appendUInt32(envelope.payload, transfer->itemId.value);
        appendUInt32(envelope.payload, transfer->quantity);
        appendUInt32(envelope.payload, transfer->sourceQuantity);
        appendItemDescriptor(envelope.payload, transfer->itemDescriptor);
    } else if (const auto* drop = std::get_if<ItemDropCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::ItemDrop, envelope.payload);
        appendUInt32(envelope.payload, drop->sourceId.value);
        appendUInt32(envelope.payload, drop->itemId.value);
        appendUInt32(envelope.payload, drop->quantity);
        appendUInt32(envelope.payload, drop->sourceQuantity);
        appendItemDescriptor(envelope.payload, drop->itemDescriptor);
    } else if (const auto* modal = std::get_if<SharedModalCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::SharedModal, envelope.payload);
        envelope.payload.push_back(static_cast<std::uint8_t>(modal->kind));
        envelope.payload.push_back(modal->open ? 1 : 0);
        appendUInt16(envelope.payload, 0);
    } else if (const auto* skill = std::get_if<UseSkillCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::UseSkill, envelope.payload);
        appendUInt32(envelope.payload, skill->targetId.value);
        appendInt32(envelope.payload, static_cast<std::int32_t>(skill->skill));
    } else if (const auto* itemUse = std::get_if<UseItemOnCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::UseItemOn, envelope.payload);
        appendUInt32(envelope.payload, itemUse->itemId.value);
        appendUInt32(envelope.payload, itemUse->targetId.value);
    } else if (const auto* elevator = std::get_if<ElevatorCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::Elevator, envelope.payload);
        appendInt32(envelope.payload, elevator->elevatorType);
        appendInt32(envelope.payload, elevator->destinationLevel);
    } else if (const auto* exitGrid = std::get_if<ExitGridCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::ExitGrid, envelope.payload);
        appendUInt32(envelope.payload, exitGrid->exitId.value);
    } else if (const auto* transition = std::get_if<SceneryTransitionCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::SceneryTransition, envelope.payload);
        appendUInt32(envelope.payload, transition->transitionId.value);
    } else if (const auto* rest = std::get_if<RestCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::Rest, envelope.payload);
        appendInt32(envelope.payload, rest->minutes);
    } else {
        const auto* attack = std::get_if<AttackCommand>(&command.payload);
        appendCommandHeader(command, CommandType::Attack, envelope.payload);
        appendUInt32(envelope.payload, attack->targetId.value);
        appendInt32(envelope.payload, attack->hitMode);
        appendInt32(envelope.payload, attack->hitLocation);
    }
    return GameplayWireError::None;
}

GameCommandDecodeResult decodeGameCommand(const ProtocolEnvelope& envelope)
{
    GameCommandDecodeResult result;
    result.error = validateEnvelope(envelope, MessageKind::Command);
    if (result.error != GameplayWireError::None) {
        return result;
    }
    if (envelope.payload.size() < kCommandHeaderSize) {
        result.error = GameplayWireError::InvalidLength;
        return result;
    }
    if (readUInt16(envelope.payload, 0) != kGameplayWireVersion) {
        result.error = GameplayWireError::UnsupportedVersion;
        return result;
    }
    if (envelope.payload[3] != 0 || envelope.payload[21] != 0 || envelope.payload[22] != 0 || envelope.payload[23] != 0) {
        result.error = GameplayWireError::InvalidReservedField;
        return result;
    }

    CommandType type = static_cast<CommandType>(envelope.payload[2]);
    result.command.sequence.value = readUInt64(envelope.payload, 4);
    result.command.playerId.value = readUInt32(envelope.payload, 12);
    result.command.actorId.value = readUInt32(envelope.payload, 16);
    result.command.expectedPhase = static_cast<SessionPhase>(envelope.payload[20]);
    result.command.expectedPhaseRevision = readUInt32(envelope.payload, 24);

    switch (type) {
    case CommandType::Move:
        if (envelope.payload.size() != kMoveCommandSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        if (envelope.payload[36] > 1 || envelope.payload[37] != 0 || envelope.payload[38] != 0 || envelope.payload[39] != 0) {
            result.error = GameplayWireError::InvalidReservedField;
            return result;
        }
        result.command.payload = MoveCommand {
            readInt32(envelope.payload, 28),
            readInt32(envelope.payload, 32),
            envelope.payload[36] != 0,
        };
        break;
    case CommandType::Face:
        if (envelope.payload.size() != kFacingCommandSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        if (readUInt32(envelope.payload, 32) != 0) {
            result.error = GameplayWireError::InvalidReservedField;
            return result;
        }
        result.command.payload = FaceCommand { readInt32(envelope.payload, 28) };
        break;
    case CommandType::UseDoor:
    case CommandType::Pickup:
    case CommandType::Loot: {
        if (envelope.payload.size() != kTargetCommandSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        EntityId targetId { readUInt32(envelope.payload, 28) };
        if (type == CommandType::UseDoor) {
            result.command.payload = InteractCommand { targetId };
        } else if (type == CommandType::Pickup) {
            result.command.payload = PickupCommand { targetId };
        } else {
            result.command.payload = LootCommand { targetId };
        }
        break;
    }
    case CommandType::InventoryTransfer:
        if (envelope.payload.size() != kInventoryTransferCommandSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.command.payload = InventoryTransferCommand {
            EntityId { readUInt32(envelope.payload, 28) },
            EntityId { readUInt32(envelope.payload, 32) },
            EntityId { readUInt32(envelope.payload, 36) },
            readUInt32(envelope.payload, 40),
            readUInt32(envelope.payload, 44),
            readItemDescriptor(envelope.payload, 48),
        };
        break;
    case CommandType::ItemDrop:
        if (envelope.payload.size() != kItemDropCommandSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.command.payload = ItemDropCommand {
            EntityId { readUInt32(envelope.payload, 28) },
            EntityId { readUInt32(envelope.payload, 32) },
            readUInt32(envelope.payload, 36),
            readUInt32(envelope.payload, 40),
            readItemDescriptor(envelope.payload, 44),
        };
        break;
    case CommandType::Attack:
        if (envelope.payload.size() != kAttackCommandSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.command.payload = AttackCommand {
            EntityId { readUInt32(envelope.payload, 28) },
            readInt32(envelope.payload, 32),
            readInt32(envelope.payload, 36),
        };
        break;
    case CommandType::SharedModal:
        if (envelope.payload.size() != kSharedModalCommandSize
            || envelope.payload[29] > 1
            || envelope.payload[30] != 0
            || envelope.payload[31] != 0) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.command.payload = SharedModalCommand {
            static_cast<SharedModalKind>(envelope.payload[28]),
            envelope.payload[29] != 0,
        };
        break;
    case CommandType::UseSkill:
        if (envelope.payload.size() != kSkillCommandSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.command.payload = UseSkillCommand {
            EntityId { readUInt32(envelope.payload, 28) },
            static_cast<ExplorationSkill>(readInt32(envelope.payload, 32)),
        };
        break;
    case CommandType::UseItemOn:
        if (envelope.payload.size() != kItemUseCommandSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.command.payload = UseItemOnCommand {
            EntityId { readUInt32(envelope.payload, 28) },
            EntityId { readUInt32(envelope.payload, 32) },
        };
        break;
    case CommandType::Elevator:
        if (envelope.payload.size() != kElevatorCommandSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.command.payload = ElevatorCommand {
            readInt32(envelope.payload, 28),
            readInt32(envelope.payload, 32),
        };
        break;
    case CommandType::ExitGrid:
        if (envelope.payload.size() != kTargetCommandSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.command.payload = ExitGridCommand {
            EntityId { readUInt32(envelope.payload, 28) },
        };
        break;
    case CommandType::SceneryTransition:
        if (envelope.payload.size() != kTargetCommandSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.command.payload = SceneryTransitionCommand {
            EntityId { readUInt32(envelope.payload, 28) },
        };
        break;
    case CommandType::Rest:
        if (envelope.payload.size() != kTargetCommandSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.command.payload = RestCommand { readInt32(envelope.payload, 28) };
        break;
    default:
        result.error = GameplayWireError::UnknownPayloadType;
        return result;
    }

    result.error = validateCommand(result.command);
    return result;
}

GameplayWireError encodeCommandResult(const CommandResult& result, ProtocolEnvelope& envelope)
{
    GameplayWireError error = validateEnvelopeIdentity(envelope);
    if (error != GameplayWireError::None) {
        return error;
    }
    error = validateResult(result);
    if (error != GameplayWireError::None) {
        return error;
    }

    envelope.kind = MessageKind::CommandResult;
    envelope.payload.clear();
    appendUInt16(envelope.payload, kGameplayWireVersion);
    envelope.payload.push_back(static_cast<std::uint8_t>(result.status));
    envelope.payload.push_back(static_cast<std::uint8_t>(result.rejection));
    appendUInt64(envelope.payload, result.commandSequence.value);
    appendUInt64(envelope.payload, result.firstEventSequence.value);
    appendUInt32(envelope.payload, result.eventCount);
    return GameplayWireError::None;
}

CommandResultDecodeResult decodeCommandResult(const ProtocolEnvelope& envelope)
{
    CommandResultDecodeResult result;
    result.error = validateEnvelope(envelope, MessageKind::CommandResult);
    if (result.error != GameplayWireError::None) {
        return result;
    }
    if (envelope.payload.size() != kCommandResultSize) {
        result.error = GameplayWireError::InvalidLength;
        return result;
    }
    if (readUInt16(envelope.payload, 0) != kGameplayWireVersion) {
        result.error = GameplayWireError::UnsupportedVersion;
        return result;
    }

    result.result.status = static_cast<CommandStatus>(envelope.payload[2]);
    result.result.rejection = static_cast<CommandRejection>(envelope.payload[3]);
    result.result.commandSequence.value = readUInt64(envelope.payload, 4);
    result.result.firstEventSequence.value = readUInt64(envelope.payload, 12);
    result.result.eventCount = readUInt32(envelope.payload, 20);
    result.error = validateResult(result.result);
    return result;
}

GameplayWireError encodeGameEvent(const GameEvent& event, ProtocolEnvelope& envelope)
{
    GameplayWireError error = validateEnvelopeIdentity(envelope);
    if (error != GameplayWireError::None) {
        return error;
    }
    error = validateEvent(event);
    if (error != GameplayWireError::None) {
        return error;
    }

    envelope.kind = MessageKind::Event;
    envelope.payload.clear();
    if (const auto* movement = std::get_if<ActorMovementStartedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::ActorMovementStarted, envelope.payload);
        appendUInt32(envelope.payload, movement->actorId.value);
        appendInt32(envelope.payload, movement->destinationTile);
        appendInt32(envelope.payload, movement->elevation);
        appendInt32(envelope.payload, movement->startingTile);
        envelope.payload.push_back(movement->running ? 1 : 0);
        envelope.payload.push_back(0);
        appendUInt16(envelope.payload, static_cast<std::uint16_t>(movement->path.size()));
        envelope.payload.insert(envelope.payload.end(), movement->path.begin(), movement->path.end());
    } else if (const auto* facing = std::get_if<ActorFacingChangedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::ActorFacingChanged, envelope.payload);
        appendUInt32(envelope.payload, facing->actorId.value);
        appendInt32(envelope.payload, facing->rotation);
    } else if (const auto* door = std::get_if<DoorUseStartedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::DoorUseStarted, envelope.payload);
        appendUInt32(envelope.payload, door->actorId.value);
        appendUInt32(envelope.payload, door->targetId.value);
        envelope.payload.push_back(door->open ? 1 : 0);
        envelope.payload.push_back(door->locked ? 1 : 0);
        appendUInt16(envelope.payload, 0);
        appendInt32(envelope.payload, door->frame);
    } else if (const auto* pickup = std::get_if<ItemPickupStartedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::ItemPickupStarted, envelope.payload);
        appendUInt32(envelope.payload, pickup->actorId.value);
        appendUInt32(envelope.payload, pickup->targetId.value);
    } else if (const auto* pickup = std::get_if<ItemPickupCompletedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::ItemPickupCompleted, envelope.payload);
        appendUInt32(envelope.payload, pickup->actorId.value);
        appendUInt32(envelope.payload, pickup->targetId.value);
        envelope.payload.push_back(pickup->succeeded ? 1 : 0);
        envelope.payload.insert(envelope.payload.end(), 3, 0);
        appendUInt32(envelope.payload, pickup->quantity);
        appendItemDescriptor(envelope.payload, pickup->itemDescriptor);
    } else if (const auto* loot = std::get_if<LootStartedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::LootStarted, envelope.payload);
        appendUInt32(envelope.payload, loot->actorId.value);
        appendUInt32(envelope.payload, loot->targetId.value);
    } else if (const auto* transfer = std::get_if<InventoryTransferredEvent>(&event.payload)) {
        appendEventHeader(event, EventType::InventoryTransferred, envelope.payload);
        appendUInt32(envelope.payload, transfer->actorId.value);
        appendUInt32(envelope.payload, transfer->sourceId.value);
        appendUInt32(envelope.payload, transfer->destinationId.value);
        appendUInt32(envelope.payload, transfer->itemId.value);
        appendUInt32(envelope.payload, transfer->quantity);
        appendUInt32(envelope.payload, transfer->sourceQuantity);
        appendUInt32(envelope.payload, transfer->remainderItemId.value);
        appendItemDescriptor(envelope.payload, transfer->itemDescriptor);
    } else if (const auto* drop = std::get_if<ItemDroppedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::ItemDropped, envelope.payload);
        appendUInt32(envelope.payload, drop->actorId.value);
        appendUInt32(envelope.payload, drop->sourceId.value);
        appendUInt32(envelope.payload, drop->itemId.value);
        appendUInt32(envelope.payload, drop->quantity);
        appendUInt32(envelope.payload, drop->sourceQuantity);
        appendUInt32(envelope.payload, drop->remainderItemId.value);
        appendInt32(envelope.payload, drop->tile);
        appendInt32(envelope.payload, drop->elevation);
        appendItemDescriptor(envelope.payload, drop->itemDescriptor);
    } else if (const auto* modal = std::get_if<SharedModalStateChangedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::SharedModalStateChanged, envelope.payload);
        appendUInt32(envelope.payload, modal->actorId.value);
        envelope.payload.push_back(static_cast<std::uint8_t>(modal->kind));
        envelope.payload.push_back(modal->open ? 1 : 0);
        envelope.payload.push_back(static_cast<std::uint8_t>(modal->phase));
        envelope.payload.push_back(0);
        appendUInt32(envelope.payload, modal->phaseRevision);
    } else if (const auto* skill = std::get_if<SkillUseStartedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::SkillUseStarted, envelope.payload);
        appendUInt32(envelope.payload, skill->actorId.value);
        appendUInt32(envelope.payload, skill->targetId.value);
        appendInt32(envelope.payload, static_cast<std::int32_t>(skill->skill));
    } else if (const auto* itemUse = std::get_if<ItemUseStartedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::ItemUseStarted, envelope.payload);
        appendUInt32(envelope.payload, itemUse->actorId.value);
        appendUInt32(envelope.payload, itemUse->itemId.value);
        appendUInt32(envelope.payload, itemUse->targetId.value);
    } else if (const auto* elevator = std::get_if<ElevatorTransitionedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::ElevatorTransitioned, envelope.payload);
        appendUInt32(envelope.payload, elevator->actorId.value);
        appendInt32(envelope.payload, elevator->elevatorType);
        appendInt32(envelope.payload, elevator->map);
        appendInt32(envelope.payload, elevator->hostTile);
        appendInt32(envelope.payload, elevator->hostElevation);
        appendInt32(envelope.payload, elevator->hostRotation);
        appendInt32(envelope.payload, elevator->guestTile);
        appendInt32(envelope.payload, elevator->guestElevation);
        appendInt32(envelope.payload, elevator->guestRotation);
        appendUInt32(envelope.payload, elevator->phaseRevision);
    } else if (const auto* exitGrid = std::get_if<ExitGridTransitionedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::ExitGridTransitioned, envelope.payload);
        appendUInt32(envelope.payload, exitGrid->actorId.value);
        appendUInt32(envelope.payload, exitGrid->exitId.value);
        appendInt32(envelope.payload, exitGrid->map);
        appendUInt32(envelope.payload, exitGrid->phaseRevision);
        appendUInt32(envelope.payload, static_cast<std::uint32_t>(exitGrid->placements.size()));
        for (const PlayerTransitionPlacement& placement : exitGrid->placements) {
            appendUInt32(envelope.payload, placement.playerId.value);
            appendUInt32(envelope.payload, placement.actorId.value);
            appendInt32(envelope.payload, placement.tile);
            appendInt32(envelope.payload, placement.elevation);
            appendInt32(envelope.payload, placement.rotation);
        }
    } else if (const auto* transition = std::get_if<SceneryTransitionedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::SceneryTransitioned, envelope.payload);
        appendUInt32(envelope.payload, transition->actorId.value);
        appendUInt32(envelope.payload, transition->transitionId.value);
        appendInt32(envelope.payload, transition->map);
        appendUInt32(envelope.payload, transition->phaseRevision);
        appendUInt32(envelope.payload, static_cast<std::uint32_t>(transition->placements.size()));
        for (const PlayerTransitionPlacement& placement : transition->placements) {
            appendUInt32(envelope.payload, placement.playerId.value);
            appendUInt32(envelope.payload, placement.actorId.value);
            appendInt32(envelope.payload, placement.tile);
            appendInt32(envelope.payload, placement.elevation);
            appendInt32(envelope.payload, placement.rotation);
        }
    } else if (const auto* rest = std::get_if<RestStateChangedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::RestStateChanged, envelope.payload);
        appendUInt32(envelope.payload, rest->actorId.value);
        appendInt32(envelope.payload, rest->minutes);
        appendUInt32(envelope.payload, rest->completed ? 1 : 0);
        appendInt32(envelope.payload, rest->gameTime);
        appendUInt32(envelope.payload, rest->phaseRevision);
        appendUInt32(envelope.payload, rest->interrupted ? 1 : 0);
    } else {
        const auto* attack = std::get_if<AttackStartedEvent>(&event.payload);
        appendEventHeader(event, EventType::AttackStarted, envelope.payload);
        appendUInt32(envelope.payload, attack->actorId.value);
        appendUInt32(envelope.payload, attack->targetId.value);
        appendInt32(envelope.payload, attack->hitMode);
        appendInt32(envelope.payload, attack->hitLocation);
    }
    return GameplayWireError::None;
}

GameEventDecodeResult decodeGameEvent(const ProtocolEnvelope& envelope)
{
    GameEventDecodeResult result;
    result.error = validateEnvelope(envelope, MessageKind::Event);
    if (result.error != GameplayWireError::None) {
        return result;
    }
    if (envelope.payload.size() < kEventHeaderSize) {
        result.error = GameplayWireError::InvalidLength;
        return result;
    }
    if (readUInt16(envelope.payload, 0) != kGameplayWireVersion) {
        result.error = GameplayWireError::UnsupportedVersion;
        return result;
    }
    if (envelope.payload[3] != 0) {
        result.error = GameplayWireError::InvalidReservedField;
        return result;
    }

    EventType type = static_cast<EventType>(envelope.payload[2]);
    result.event.sequence.value = readUInt64(envelope.payload, 4);
    result.event.causedBy.value = readUInt64(envelope.payload, 12);
    switch (type) {
    case EventType::ActorMovementStarted:
        if (envelope.payload.size() < kMovementEventHeaderSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        if (envelope.payload[36] > 1 || envelope.payload[37] != 0) {
            result.error = GameplayWireError::InvalidReservedField;
            return result;
        }
        {
            std::size_t pathLength = readUInt16(envelope.payload, 38);
            if (pathLength > kMaximumMovementPathLength
                || envelope.payload.size() != kMovementEventHeaderSize + pathLength) {
                result.error = GameplayWireError::InvalidLength;
                return result;
            }
            result.event.payload = ActorMovementStartedEvent {
                EntityId { readUInt32(envelope.payload, 20) },
                readInt32(envelope.payload, 24),
                readInt32(envelope.payload, 28),
                envelope.payload[36] != 0,
                readInt32(envelope.payload, 32),
                std::vector<std::uint8_t>(envelope.payload.begin() + kMovementEventHeaderSize, envelope.payload.end()),
            };
        }
        break;
    case EventType::ActorFacingChanged:
        if (envelope.payload.size() != kFacingEventSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.event.payload = ActorFacingChangedEvent {
            EntityId { readUInt32(envelope.payload, 20) },
            readInt32(envelope.payload, 24),
        };
        break;
    case EventType::DoorUseStarted: {
        if (envelope.payload.size() != kDoorStateEventSize
            || envelope.payload[30] != 0
            || envelope.payload[31] != 0
            || envelope.payload[28] > 1
            || envelope.payload[29] > 1) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.event.payload = DoorUseStartedEvent {
            EntityId { readUInt32(envelope.payload, 20) },
            EntityId { readUInt32(envelope.payload, 24) },
            envelope.payload[28] != 0,
            envelope.payload[29] != 0,
            readInt32(envelope.payload, 32),
        };
        break;
    }
    case EventType::ItemPickupStarted:
    case EventType::LootStarted: {
        if (envelope.payload.size() != kTargetEventSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        EntityId actorId { readUInt32(envelope.payload, 20) };
        EntityId targetId { readUInt32(envelope.payload, 24) };
        if (type == EventType::ItemPickupStarted) {
            result.event.payload = ItemPickupStartedEvent { actorId, targetId };
        } else {
            result.event.payload = LootStartedEvent { actorId, targetId };
        }
        break;
    }
    case EventType::ItemPickupCompleted:
        if (envelope.payload.size() != kPickupCompletedEventSize
            || envelope.payload[28] > 1
            || envelope.payload[29] != 0
            || envelope.payload[30] != 0
            || envelope.payload[31] != 0) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.event.payload = ItemPickupCompletedEvent {
            EntityId { readUInt32(envelope.payload, 20) },
            EntityId { readUInt32(envelope.payload, 24) },
            envelope.payload[28] != 0,
            readUInt32(envelope.payload, 32),
            readItemDescriptor(envelope.payload, 36),
        };
        break;
    case EventType::InventoryTransferred:
        if (envelope.payload.size() != kInventoryTransferEventSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.event.payload = InventoryTransferredEvent {
            EntityId { readUInt32(envelope.payload, 20) },
            EntityId { readUInt32(envelope.payload, 24) },
            EntityId { readUInt32(envelope.payload, 28) },
            EntityId { readUInt32(envelope.payload, 32) },
            readUInt32(envelope.payload, 36),
            readUInt32(envelope.payload, 40),
            EntityId { readUInt32(envelope.payload, 44) },
            readItemDescriptor(envelope.payload, 48),
        };
        break;
    case EventType::ItemDropped:
        if (envelope.payload.size() != kItemDropEventSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.event.payload = ItemDroppedEvent {
            EntityId { readUInt32(envelope.payload, 20) },
            EntityId { readUInt32(envelope.payload, 24) },
            EntityId { readUInt32(envelope.payload, 28) },
            readUInt32(envelope.payload, 32),
            readUInt32(envelope.payload, 36),
            EntityId { readUInt32(envelope.payload, 40) },
            readInt32(envelope.payload, 44),
            readInt32(envelope.payload, 48),
            readItemDescriptor(envelope.payload, 52),
        };
        break;
    case EventType::AttackStarted:
        if (envelope.payload.size() != kAttackEventSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.event.payload = AttackStartedEvent {
            EntityId { readUInt32(envelope.payload, 20) },
            EntityId { readUInt32(envelope.payload, 24) },
            readInt32(envelope.payload, 28),
            readInt32(envelope.payload, 32),
        };
        break;
    case EventType::SharedModalStateChanged:
        if (envelope.payload.size() != kSharedModalEventSize
            || envelope.payload[25] > 1
            || envelope.payload[27] != 0) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.event.payload = SharedModalStateChangedEvent {
            EntityId { readUInt32(envelope.payload, 20) },
            static_cast<SharedModalKind>(envelope.payload[24]),
            envelope.payload[25] != 0,
            static_cast<SessionPhase>(envelope.payload[26]),
            readUInt32(envelope.payload, 28),
        };
        break;
    case EventType::SkillUseStarted:
        if (envelope.payload.size() != kSkillEventSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.event.payload = SkillUseStartedEvent {
            EntityId { readUInt32(envelope.payload, 20) },
            EntityId { readUInt32(envelope.payload, 24) },
            static_cast<ExplorationSkill>(readInt32(envelope.payload, 28)),
        };
        break;
    case EventType::ItemUseStarted:
        if (envelope.payload.size() != kItemUseEventSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.event.payload = ItemUseStartedEvent {
            EntityId { readUInt32(envelope.payload, 20) },
            EntityId { readUInt32(envelope.payload, 24) },
            EntityId { readUInt32(envelope.payload, 28) },
        };
        break;
    case EventType::ElevatorTransitioned:
        if (envelope.payload.size() != kElevatorEventSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.event.payload = ElevatorTransitionedEvent {
            EntityId { readUInt32(envelope.payload, 20) },
            readInt32(envelope.payload, 24),
            readInt32(envelope.payload, 28),
            readInt32(envelope.payload, 32),
            readInt32(envelope.payload, 36),
            readInt32(envelope.payload, 40),
            readInt32(envelope.payload, 44),
            readInt32(envelope.payload, 48),
            readInt32(envelope.payload, 52),
            readUInt32(envelope.payload, 56),
        };
        break;
    case EventType::ExitGridTransitioned: {
        if (envelope.payload.size() < kExitGridEventBaseSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        std::uint32_t placementCount = readUInt32(envelope.payload, 36);
        if (placementCount == 0
            || placementCount > kMaximumTransitionPlayers
            || envelope.payload.size() != kExitGridEventBaseSize
                    + static_cast<std::size_t>(placementCount) * kTransitionPlacementSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        ExitGridTransitionedEvent exitGrid;
        exitGrid.actorId = EntityId { readUInt32(envelope.payload, 20) };
        exitGrid.exitId = EntityId { readUInt32(envelope.payload, 24) };
        exitGrid.map = readInt32(envelope.payload, 28);
        exitGrid.phaseRevision = readUInt32(envelope.payload, 32);
        exitGrid.placements.reserve(placementCount);
        for (std::uint32_t index = 0; index < placementCount; index++) {
            std::size_t offset = kExitGridEventBaseSize
                + static_cast<std::size_t>(index) * kTransitionPlacementSize;
            exitGrid.placements.push_back(PlayerTransitionPlacement {
                PlayerId { readUInt32(envelope.payload, offset) },
                EntityId { readUInt32(envelope.payload, offset + 4) },
                readInt32(envelope.payload, offset + 8),
                readInt32(envelope.payload, offset + 12),
                readInt32(envelope.payload, offset + 16),
            });
        }
        result.event.payload = std::move(exitGrid);
        break;
    }
    case EventType::SceneryTransitioned: {
        if (envelope.payload.size() < kExitGridEventBaseSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        std::uint32_t placementCount = readUInt32(envelope.payload, 36);
        if (placementCount == 0
            || placementCount > kMaximumTransitionPlayers
            || envelope.payload.size() != kExitGridEventBaseSize
                    + static_cast<std::size_t>(placementCount) * kTransitionPlacementSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        SceneryTransitionedEvent transition;
        transition.actorId = EntityId { readUInt32(envelope.payload, 20) };
        transition.transitionId = EntityId { readUInt32(envelope.payload, 24) };
        transition.map = readInt32(envelope.payload, 28);
        transition.phaseRevision = readUInt32(envelope.payload, 32);
        transition.placements.reserve(placementCount);
        for (std::uint32_t index = 0; index < placementCount; index++) {
            std::size_t offset = kExitGridEventBaseSize
                + static_cast<std::size_t>(index) * kTransitionPlacementSize;
            transition.placements.push_back(PlayerTransitionPlacement {
                PlayerId { readUInt32(envelope.payload, offset) },
                EntityId { readUInt32(envelope.payload, offset + 4) },
                readInt32(envelope.payload, offset + 8),
                readInt32(envelope.payload, offset + 12),
                readInt32(envelope.payload, offset + 16),
            });
        }
        result.event.payload = std::move(transition);
        break;
    }
    case EventType::RestStateChanged:
        if (envelope.payload.size() != kRestEventSize
            || readUInt32(envelope.payload, 28) > 1
            || readUInt32(envelope.payload, 40) > 1) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.event.payload = RestStateChangedEvent {
            EntityId { readUInt32(envelope.payload, 20) },
            readInt32(envelope.payload, 24),
            readUInt32(envelope.payload, 28) != 0,
            readInt32(envelope.payload, 32),
            readUInt32(envelope.payload, 36),
            readUInt32(envelope.payload, 40) != 0,
        };
        break;
    default:
        result.error = GameplayWireError::UnknownPayloadType;
        return result;
    }

    result.error = validateEvent(result.event);
    return result;
}

const char* gameplayWireErrorMessage(GameplayWireError error)
{
    switch (error) {
    case GameplayWireError::None:
        return "no error";
    case GameplayWireError::WrongMessageKind:
        return "wrong protocol message kind";
    case GameplayWireError::InvalidSessionId:
        return "invalid session ID";
    case GameplayWireError::InvalidEnvelopeSequence:
        return "invalid envelope sequence";
    case GameplayWireError::UnsupportedVersion:
        return "unsupported gameplay format version";
    case GameplayWireError::UnknownPayloadType:
        return "unknown gameplay payload type";
    case GameplayWireError::InvalidLength:
        return "invalid gameplay payload length";
    case GameplayWireError::InvalidReservedField:
        return "invalid reserved field";
    case GameplayWireError::InvalidCommandSequence:
        return "invalid command sequence";
    case GameplayWireError::InvalidEventSequence:
        return "invalid event sequence";
    case GameplayWireError::InvalidPlayerId:
        return "invalid player ID";
    case GameplayWireError::InvalidEntityId:
        return "invalid entity ID";
    case GameplayWireError::InvalidPhase:
        return "invalid session phase";
    case GameplayWireError::InvalidPhaseRevision:
        return "invalid phase revision";
    case GameplayWireError::InvalidMove:
        return "invalid movement payload";
    case GameplayWireError::InvalidRotation:
        return "invalid actor rotation";
    case GameplayWireError::InvalidSkill:
        return "invalid exploration skill";
    case GameplayWireError::InvalidAttack:
        return "invalid attack payload";
    case GameplayWireError::InvalidModal:
        return "invalid shared modal payload";
    case GameplayWireError::InvalidQuantity:
        return "invalid inventory quantity";
    case GameplayWireError::InvalidStatus:
        return "invalid command status";
    case GameplayWireError::InvalidRejection:
        return "invalid command rejection";
    case GameplayWireError::InconsistentResult:
        return "inconsistent command result";
    }
    return "unknown gameplay wire error";
}

} // namespace multiplayer
} // namespace fallout
