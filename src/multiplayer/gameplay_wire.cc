#include "multiplayer/gameplay_wire.h"

#include <limits>
#include <unordered_set>
#include <variant>

#include "multiplayer/combat_turn_controller.h"
#include "multiplayer/direct_trade_controller.h"

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
    WorldMapRoute = 16,
    EndTurn = 17,
    CombatMove = 18,
    CombatItem = 19,
    CombatReload = 20,
    CombatFace = 21,
    Talk = 22,
    DialogueVote = 23,
    DirectTrade = 24,
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
    WorldMapRouteSelected = 17,
    WorldMapArrived = 18,
    CombatTurnStateChanged = 19,
    CombatActionResolved = 20,
    PartyExperienceAwarded = 21,
    DialogueRequested = 22,
    DialogueVoteRecorded = 23,
    DialoguePresentation = 24,
    SharedActivityPublished = 25,
    DirectTradeStateChanged = 26,
};

constexpr std::size_t kCommandHeaderSize = 28;
constexpr std::size_t kMoveCommandSize = kCommandHeaderSize + 12;
constexpr std::size_t kTargetCommandSize = kCommandHeaderSize + 4;
constexpr std::size_t kFacingCommandSize = kCommandHeaderSize + 8;
constexpr std::size_t kItemDescriptorSize = 16;
constexpr std::size_t kInventoryTransferCommandSize = kCommandHeaderSize + 20 + kItemDescriptorSize;
constexpr std::size_t kDirectTradeCommandSize = kCommandHeaderSize + 28;
constexpr std::size_t kItemDropCommandSize = kCommandHeaderSize + 16 + kItemDescriptorSize;
constexpr std::size_t kAttackCommandSize = kCommandHeaderSize + 20;
constexpr std::size_t kEndTurnCommandSize = kCommandHeaderSize + 8;
constexpr std::size_t kCombatMoveCommandSize = kCommandHeaderSize + 20;
constexpr std::size_t kCombatItemCommandSize = kCommandHeaderSize + 16;
constexpr std::size_t kCombatReloadCommandSize = kCommandHeaderSize + 16;
constexpr std::size_t kCombatFaceCommandSize = kCommandHeaderSize + 12;
constexpr std::size_t kSharedModalCommandSize = kCommandHeaderSize + 4;
constexpr std::size_t kDialogueVoteCommandSize = kCommandHeaderSize + 12;
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
constexpr std::size_t kCombatTurnEventBaseSize = kEventHeaderSize + 40;
constexpr std::size_t kCombatActionEventSize = kEventHeaderSize + 24;
constexpr std::size_t kPartyExperienceEventBaseSize = kEventHeaderSize + 12;
constexpr std::size_t kPlayerProgressionSize = 20;
constexpr std::size_t kSharedModalEventSize = kEventHeaderSize + 12;
constexpr std::size_t kDialogueVoteEventSize = kEventHeaderSize + 16;
constexpr std::size_t kSkillEventSize = kEventHeaderSize + 12;
constexpr std::size_t kItemUseEventSize = kEventHeaderSize + 12;
constexpr std::size_t kElevatorEventSize = kEventHeaderSize + 40;
constexpr std::size_t kExitGridEventBaseSize = kEventHeaderSize + 20;
constexpr std::size_t kRestEventSize = kEventHeaderSize + 24;
constexpr std::size_t kWorldMapRouteCommandSize = kCommandHeaderSize + 12;
constexpr std::size_t kWorldMapRouteEventSize = kEventHeaderSize + 16;
constexpr std::size_t kWorldMapArrivalEventBaseSize = kEventHeaderSize + 36;
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
    if (const auto* trade = std::get_if<DirectTradeCommand>(&command.payload)) {
        bool base = trade->caps <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
            && trade->quantity <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
        bool valid = false;
        switch (trade->action) {
        case DirectTradeAction::Open:
            valid = isValid(trade->partnerId) && trade->partnerId != command.playerId
                && trade->revision == 0 && !isValid(trade->itemId)
                && trade->quantity == 0 && trade->caps == 0;
            break;
        case DirectTradeAction::SetCaps:
            valid = !isValid(trade->partnerId) && trade->revision != 0
                && !isValid(trade->itemId) && trade->quantity == 0;
            break;
        case DirectTradeAction::SetItem:
            valid = !isValid(trade->partnerId) && trade->revision != 0
                && isValid(trade->itemId) && trade->caps == 0;
            break;
        case DirectTradeAction::Confirm:
        case DirectTradeAction::Cancel:
            valid = !isValid(trade->partnerId) && trade->revision != 0
                && !isValid(trade->itemId) && trade->quantity == 0 && trade->caps == 0;
            break;
        }
        return base && valid ? GameplayWireError::None : GameplayWireError::InvalidQuantity;
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
        return attack->turnRevision != 0
                && attack->hitMode >= 0 && attack->hitMode < kAttackHitModeCount
                && attack->hitMode != 6 && attack->hitMode != 7
                && attack->hitLocation >= 0 && attack->hitLocation < kAttackHitLocationCount
            ? GameplayWireError::None
            : GameplayWireError::InvalidAttack;
    }
    if (const auto* endTurn = std::get_if<EndTurnCommand>(&command.payload)) {
        return endTurn->turnRevision != 0
            ? GameplayWireError::None
            : GameplayWireError::InvalidCombatTurn;
    }
    if (const auto* move = std::get_if<CombatMoveCommand>(&command.payload)) {
        return move->turnRevision != 0 && move->destinationTile >= 0
                && move->elevation >= 0 && move->elevation <= 2
            ? GameplayWireError::None : GameplayWireError::InvalidCombatTurn;
    }
    if (const auto* item = std::get_if<CombatItemCommand>(&command.payload)) {
        return item->turnRevision != 0 && isValid(item->itemId)
                && item->itemId != item->targetId
            ? GameplayWireError::None : GameplayWireError::InvalidCombatTurn;
    }
    if (const auto* reload = std::get_if<CombatReloadCommand>(&command.payload)) {
        return reload->turnRevision != 0 && isValid(reload->weaponId)
                && (reload->hitMode == 6 || reload->hitMode == 7)
            ? GameplayWireError::None : GameplayWireError::InvalidCombatTurn;
    }
    if (const auto* face = std::get_if<CombatFaceCommand>(&command.payload)) {
        return face->turnRevision != 0 && face->rotation >= 0
                && face->rotation < kActorRotationCount
            ? GameplayWireError::None : GameplayWireError::InvalidCombatTurn;
    }
    if (const auto* modal = std::get_if<SharedModalCommand>(&command.payload)) {
        return isValid(modal->kind)
            ? GameplayWireError::None
            : GameplayWireError::InvalidModal;
    }
    if (const auto* talk = std::get_if<TalkCommand>(&command.payload)) {
        return isValid(talk->targetId) ? GameplayWireError::None : GameplayWireError::InvalidEntityId;
    }
    if (const auto* vote = std::get_if<DialogueVoteCommand>(&command.payload)) {
        return vote->revision != 0 && vote->option < kMaximumDialogueOptions
            ? GameplayWireError::None : GameplayWireError::InvalidModal;
    }
    if (const auto* route = std::get_if<WorldMapRouteCommand>(&command.payload)) {
        return isValid(*route) ? GameplayWireError::None : GameplayWireError::InvalidMove;
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
                && attack->hitMode != 6 && attack->hitMode != 7
                && attack->hitLocation >= 0 && attack->hitLocation < kAttackHitLocationCount
            ? GameplayWireError::None
            : GameplayWireError::InvalidAttack;
    }
    if (const auto* combat = std::get_if<CombatTurnStateChangedEvent>(&event.payload)) {
        return isValid(combat->actorId) && combat->phaseRevision != 0
                && (combat->phase == SessionPhase::Combat
                    || combat->phase == SessionPhase::Exploration)
                && isValidCombatTurnState(combat->state, combat->phase)
            ? GameplayWireError::None
            : GameplayWireError::InvalidCombatTurn;
    }
    if (const auto* action = std::get_if<CombatActionResolvedEvent>(&event.payload)) {
        return isValid(action->actorId) && isValid(action->kind)
                && action->turnRevision != 0 && action->phaseRevision != 0
                && ((action->kind == CombatActionKind::Move
                        || action->kind == CombatActionKind::Face)
                    ? !isValid(action->subjectId) : isValid(action->subjectId))
            ? GameplayWireError::None : GameplayWireError::InvalidCombatTurn;
    }
    if (const auto* award = std::get_if<PartyExperienceAwardedEvent>(&event.payload)) {
        if (!isValid(award->actorId) || award->amount == 0 || award->players.empty()
            || award->players.size() > kMaximumTransitionPlayers) {
            return GameplayWireError::InvalidCombatTurn;
        }
        std::unordered_set<PlayerId, PlayerIdHash> seen;
        for (const PlayerProgressionResult& player : award->players) {
            if (!isValid(player.playerId) || !isValid(player.actorId)
                || !seen.insert(player.playerId).second
                || player.experience < 0 || player.level < 1
                || player.unspentSkillPoints < 0) {
                return GameplayWireError::InvalidCombatTurn;
            }
        }
        return GameplayWireError::None;
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
        return (modal->phase == expectedPhase
                   || (modal->kind == SharedModalKind::WorldMap
                       && modal->open
                       && modal->phase == SessionPhase::Exploration))
            ? GameplayWireError::None
            : GameplayWireError::InvalidModal;
    }
    if (const auto* requested = std::get_if<DialogueRequestedEvent>(&event.payload)) {
        return isValid(requested->actorId) && isValid(requested->targetId)
                && requested->phaseRevision != 0
            ? GameplayWireError::None : GameplayWireError::InvalidEntityId;
    }
    if (const auto* vote = std::get_if<DialogueVoteRecordedEvent>(&event.payload)) {
        return isValid(vote->actorId) && vote->revision != 0 && vote->option < kMaximumDialogueOptions
            ? GameplayWireError::None : GameplayWireError::InvalidModal;
    }
    if (const auto* presentation = std::get_if<DialoguePresentationEvent>(&event.payload)) {
        if (!isValid(presentation->actorId) || !isValid(presentation->targetId)
            || presentation->revision == 0 || presentation->policy < 1
            || presentation->policy > 4 || presentation->reply.empty()
            || presentation->reply.size() > 899 || presentation->options.empty()
            || presentation->options.size() > kMaximumDialogueOptions) return GameplayWireError::InvalidModal;
        for (const std::string& option : presentation->options) {
            if (option.empty() || option.size() > 899) return GameplayWireError::InvalidModal;
        }
        return GameplayWireError::None;
    }
    if (const auto* activity = std::get_if<SharedActivityPublishedEvent>(&event.payload)) {
        const SharedActivityEntry& entry = activity->entry;
        return isValid(activity->actorId) && entry.id != 0
                && entry.kind >= SharedActivityKind::Quest
                && entry.kind <= SharedActivityKind::WorldOutcome
                && !entry.sourceName.empty() && entry.sourceName.size() <= 32
                && !entry.text.empty() && entry.text.size() <= 160
            ? GameplayWireError::None : GameplayWireError::InvalidModal;
    }
    if (const auto* trade = std::get_if<DirectTradeStateChangedEvent>(&event.payload)) {
        return isValid(trade->actorId)
                && !(trade->committed && trade->cancelled)
                && isValidDirectTradeState(trade->state)
                && (!(trade->committed || trade->cancelled) || trade->state.revision == 0)
            ? GameplayWireError::None : GameplayWireError::InvalidModal;
    }
    if (const auto* route = std::get_if<WorldMapRouteSelectedEvent>(&event.payload)) {
        return isValid(route->actorId)
                && isValid(WorldMapRouteCommand { route->targetX, route->targetY, route->clear })
            ? GameplayWireError::None
            : GameplayWireError::InvalidMove;
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
    if (const auto* arrival = std::get_if<WorldMapArrivedEvent>(&event.payload)) {
        if (!isValid(arrival->actorId)
            || arrival->map < 0
            || arrival->entranceIndex < 0 || arrival->entranceIndex > 32
            || arrival->phaseRevision < 2
            || arrival->worldX < 0 || arrival->worldX >= 1400
            || arrival->worldY < 0 || arrival->worldY >= 1500
            || arrival->gameTime <= 0
            || static_cast<std::uint8_t>(arrival->kind) > static_cast<std::uint8_t>(WorldMapArrivalKind::Fatal)
            || arrival->placements.empty()
            || arrival->placements.size() > kMaximumTransitionPlayers) {
            return GameplayWireError::InvalidMove;
        }
        bool actingPlayerPresent = false;
        for (std::size_t index = 0; index < arrival->placements.size(); index++) {
            const PlayerTransitionPlacement& placement = arrival->placements[index];
            if (!isValid(placement.playerId)
                || !isValid(placement.actorId)
                || placement.tile < 0
                || placement.elevation < 0 || placement.elevation > 2
                || placement.rotation < 0 || placement.rotation >= kActorRotationCount) {
                return GameplayWireError::InvalidMove;
            }
            actingPlayerPresent = actingPlayerPresent || placement.actorId == arrival->actorId;
            for (std::size_t previous = 0; previous < index; previous++) {
                if (arrival->placements[previous].playerId == placement.playerId
                    || arrival->placements[previous].actorId == placement.actorId) {
                    return GameplayWireError::InvalidMove;
                }
            }
        }
        return actingPlayerPresent ? GameplayWireError::None : GameplayWireError::InvalidMove;
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
    } else if (const auto* trade = std::get_if<DirectTradeCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::DirectTrade, envelope.payload);
        envelope.payload.push_back(static_cast<std::uint8_t>(trade->action));
        envelope.payload.insert(envelope.payload.end(), 3, 0);
        appendUInt32(envelope.payload, trade->partnerId.value);
        appendUInt64(envelope.payload, trade->revision);
        appendUInt32(envelope.payload, trade->itemId.value);
        appendUInt32(envelope.payload, trade->quantity);
        appendUInt32(envelope.payload, trade->caps);
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
    } else if (const auto* talk = std::get_if<TalkCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::Talk, envelope.payload);
        appendUInt32(envelope.payload, talk->targetId.value);
    } else if (const auto* vote = std::get_if<DialogueVoteCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::DialogueVote, envelope.payload);
        appendUInt64(envelope.payload, vote->revision);
        appendUInt32(envelope.payload, vote->option);
    } else if (const auto* route = std::get_if<WorldMapRouteCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::WorldMapRoute, envelope.payload);
        appendInt32(envelope.payload, route->targetX);
        appendInt32(envelope.payload, route->targetY);
        envelope.payload.push_back(route->clear ? 1 : 0);
        envelope.payload.insert(envelope.payload.end(), 3, 0);
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
    } else if (const auto* endTurn = std::get_if<EndTurnCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::EndTurn, envelope.payload);
        appendUInt64(envelope.payload, endTurn->turnRevision);
    } else if (const auto* move = std::get_if<CombatMoveCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::CombatMove, envelope.payload);
        appendUInt64(envelope.payload, move->turnRevision);
        appendInt32(envelope.payload, move->destinationTile);
        appendInt32(envelope.payload, move->elevation);
        appendUInt32(envelope.payload, move->running ? 1 : 0);
    } else if (const auto* item = std::get_if<CombatItemCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::CombatItem, envelope.payload);
        appendUInt64(envelope.payload, item->turnRevision);
        appendUInt32(envelope.payload, item->itemId.value);
        appendUInt32(envelope.payload, item->targetId.value);
    } else if (const auto* reload = std::get_if<CombatReloadCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::CombatReload, envelope.payload);
        appendUInt64(envelope.payload, reload->turnRevision);
        appendUInt32(envelope.payload, reload->weaponId.value);
        appendInt32(envelope.payload, reload->hitMode);
    } else if (const auto* face = std::get_if<CombatFaceCommand>(&command.payload)) {
        appendCommandHeader(command, CommandType::CombatFace, envelope.payload);
        appendUInt64(envelope.payload, face->turnRevision);
        appendInt32(envelope.payload, face->rotation);
    } else {
        const auto* attack = std::get_if<AttackCommand>(&command.payload);
        appendCommandHeader(command, CommandType::Attack, envelope.payload);
        appendUInt32(envelope.payload, attack->targetId.value);
        appendInt32(envelope.payload, attack->hitMode);
        appendInt32(envelope.payload, attack->hitLocation);
        appendUInt64(envelope.payload, attack->turnRevision);
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
    case CommandType::DirectTrade:
        if (envelope.payload.size() != kDirectTradeCommandSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        if (envelope.payload[29] != 0 || envelope.payload[30] != 0 || envelope.payload[31] != 0) {
            result.error = GameplayWireError::InvalidReservedField;
            return result;
        }
        result.command.payload = DirectTradeCommand {
            static_cast<DirectTradeAction>(envelope.payload[28]),
            PlayerId { readUInt32(envelope.payload, 32) },
            readUInt64(envelope.payload, 36),
            EntityId { readUInt32(envelope.payload, 44) },
            readUInt32(envelope.payload, 48),
            readUInt32(envelope.payload, 52),
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
            readUInt64(envelope.payload, 40),
        };
        break;
    case CommandType::EndTurn:
        if (envelope.payload.size() != kEndTurnCommandSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.command.payload = EndTurnCommand { readUInt64(envelope.payload, 28) };
        break;
    case CommandType::CombatMove:
        if (envelope.payload.size() != kCombatMoveCommandSize
            || readUInt32(envelope.payload, 44) > 1) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.command.payload = CombatMoveCommand {
            readUInt64(envelope.payload, 28),
            readInt32(envelope.payload, 36),
            readInt32(envelope.payload, 40),
            readUInt32(envelope.payload, 44) != 0,
        };
        break;
    case CommandType::CombatItem:
        if (envelope.payload.size() != kCombatItemCommandSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.command.payload = CombatItemCommand {
            readUInt64(envelope.payload, 28), EntityId { readUInt32(envelope.payload, 36) },
            EntityId { readUInt32(envelope.payload, 40) },
        };
        break;
    case CommandType::CombatReload:
        if (envelope.payload.size() != kCombatReloadCommandSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.command.payload = CombatReloadCommand {
            readUInt64(envelope.payload, 28), EntityId { readUInt32(envelope.payload, 36) },
            readInt32(envelope.payload, 40),
        };
        break;
    case CommandType::CombatFace:
        if (envelope.payload.size() != kCombatFaceCommandSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.command.payload = CombatFaceCommand {
            readUInt64(envelope.payload, 28), readInt32(envelope.payload, 36),
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
    case CommandType::Talk:
        if (envelope.payload.size() != kTargetCommandSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.command.payload = TalkCommand { EntityId { readUInt32(envelope.payload, 28) } };
        break;
    case CommandType::DialogueVote:
        if (envelope.payload.size() != kDialogueVoteCommandSize
            || readUInt32(envelope.payload, 36) >= kMaximumDialogueOptions) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.command.payload = DialogueVoteCommand {
            readUInt64(envelope.payload, 28),
            static_cast<std::uint8_t>(readUInt32(envelope.payload, 36)),
        };
        break;
    case CommandType::WorldMapRoute:
        if (envelope.payload.size() != kWorldMapRouteCommandSize
            || envelope.payload[36] > 1
            || envelope.payload[37] != 0
            || envelope.payload[38] != 0
            || envelope.payload[39] != 0) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.command.payload = WorldMapRouteCommand {
            readInt32(envelope.payload, 28),
            readInt32(envelope.payload, 32),
            envelope.payload[36] != 0,
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
    } else if (const auto* requested = std::get_if<DialogueRequestedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::DialogueRequested, envelope.payload);
        appendUInt32(envelope.payload, requested->actorId.value);
        appendUInt32(envelope.payload, requested->targetId.value);
        appendUInt32(envelope.payload, requested->phaseRevision);
    } else if (const auto* vote = std::get_if<DialogueVoteRecordedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::DialogueVoteRecorded, envelope.payload);
        appendUInt32(envelope.payload, vote->actorId.value);
        appendUInt64(envelope.payload, vote->revision);
        appendUInt32(envelope.payload, vote->option);
    } else if (const auto* presentation = std::get_if<DialoguePresentationEvent>(&event.payload)) {
        appendEventHeader(event, EventType::DialoguePresentation, envelope.payload);
        appendUInt32(envelope.payload, presentation->actorId.value);
        appendUInt32(envelope.payload, presentation->targetId.value);
        appendUInt64(envelope.payload, presentation->revision);
        envelope.payload.push_back(presentation->policy);
        envelope.payload.push_back(static_cast<std::uint8_t>(presentation->options.size()));
        appendUInt16(envelope.payload, static_cast<std::uint16_t>(presentation->reply.size()));
        envelope.payload.insert(envelope.payload.end(), presentation->reply.begin(), presentation->reply.end());
        for (const std::string& option : presentation->options) {
            appendUInt16(envelope.payload, static_cast<std::uint16_t>(option.size()));
            envelope.payload.insert(envelope.payload.end(), option.begin(), option.end());
        }
    } else if (const auto* activity = std::get_if<SharedActivityPublishedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::SharedActivityPublished, envelope.payload);
        appendUInt32(envelope.payload, activity->actorId.value);
        appendUInt64(envelope.payload, activity->entry.id);
        appendUInt32(envelope.payload, activity->entry.sourceId.value);
        envelope.payload.push_back(static_cast<std::uint8_t>(activity->entry.kind));
        envelope.payload.push_back(static_cast<std::uint8_t>(activity->entry.sourceName.size()));
        appendUInt16(envelope.payload, static_cast<std::uint16_t>(activity->entry.text.size()));
        appendInt32(envelope.payload, activity->entry.subject);
        appendInt32(envelope.payload, activity->entry.value);
        envelope.payload.insert(envelope.payload.end(), activity->entry.sourceName.begin(), activity->entry.sourceName.end());
        envelope.payload.insert(envelope.payload.end(), activity->entry.text.begin(), activity->entry.text.end());
    } else if (const auto* trade = std::get_if<DirectTradeStateChangedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::DirectTradeStateChanged, envelope.payload);
        appendUInt32(envelope.payload, trade->actorId.value);
        envelope.payload.push_back(trade->committed ? 1 : 0);
        envelope.payload.push_back(trade->cancelled ? 1 : 0);
        envelope.payload.push_back(static_cast<std::uint8_t>(trade->state.offers.size()));
        envelope.payload.push_back(0);
        appendUInt64(envelope.payload, trade->state.revision);
        for (const DirectTradeOffer& offer : trade->state.offers) {
            appendUInt32(envelope.payload, offer.playerId.value);
            appendUInt32(envelope.payload, offer.caps);
            envelope.payload.push_back(offer.confirmed ? 1 : 0);
            envelope.payload.push_back(static_cast<std::uint8_t>(offer.items.size()));
            appendUInt16(envelope.payload, 0);
            for (const DirectTradeLine& line : offer.items) {
                appendUInt32(envelope.payload, line.itemId.value);
                appendUInt32(envelope.payload, line.quantity);
            }
        }
    } else if (const auto* route = std::get_if<WorldMapRouteSelectedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::WorldMapRouteSelected, envelope.payload);
        appendUInt32(envelope.payload, route->actorId.value);
        appendInt32(envelope.payload, route->targetX);
        appendInt32(envelope.payload, route->targetY);
        envelope.payload.push_back(route->clear ? 1 : 0);
        envelope.payload.insert(envelope.payload.end(), 3, 0);
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
    } else if (const auto* arrival = std::get_if<WorldMapArrivedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::WorldMapArrived, envelope.payload);
        appendUInt32(envelope.payload, arrival->actorId.value);
        appendInt32(envelope.payload, arrival->map);
        appendUInt32(envelope.payload, arrival->phaseRevision);
        appendInt32(envelope.payload, arrival->worldX);
        appendInt32(envelope.payload, arrival->worldY);
        appendInt32(envelope.payload, arrival->gameTime);
        appendUInt32(envelope.payload, static_cast<std::uint32_t>(arrival->kind));
        appendUInt32(envelope.payload, static_cast<std::uint32_t>(arrival->placements.size()));
        appendInt32(envelope.payload, arrival->entranceIndex);
        for (const PlayerTransitionPlacement& placement : arrival->placements) {
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
    } else if (const auto* combat = std::get_if<CombatTurnStateChangedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::CombatTurnStateChanged, envelope.payload);
        appendUInt32(envelope.payload, combat->actorId.value);
        envelope.payload.push_back(static_cast<std::uint8_t>(combat->phase));
        envelope.payload.insert(envelope.payload.end(), 3, 0);
        appendUInt32(envelope.payload, combat->phaseRevision);
        appendUInt64(envelope.payload, combat->state.revision);
        appendUInt64(envelope.payload, combat->state.round);
        appendUInt32(envelope.payload, combat->state.activeIndex);
        appendUInt32(envelope.payload, combat->state.remainingMilliseconds);
        appendUInt32(envelope.payload, static_cast<std::uint32_t>(combat->state.initiative.size()));
        for (const CombatInitiativeEntry& entry : combat->state.initiative) {
            appendUInt32(envelope.payload, entry.actorId.value);
            appendUInt32(envelope.payload, entry.ownerId.value);
        }
    } else if (const auto* action = std::get_if<CombatActionResolvedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::CombatActionResolved, envelope.payload);
        appendUInt32(envelope.payload, action->actorId.value);
        envelope.payload.push_back(static_cast<std::uint8_t>(action->kind));
        envelope.payload.insert(envelope.payload.end(), 3, 0);
        appendUInt32(envelope.payload, action->subjectId.value);
        appendUInt64(envelope.payload, action->turnRevision);
        appendUInt32(envelope.payload, action->phaseRevision);
    } else if (const auto* award = std::get_if<PartyExperienceAwardedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::PartyExperienceAwarded, envelope.payload);
        appendUInt32(envelope.payload, award->actorId.value);
        appendInt32(envelope.payload, award->amount);
        appendUInt32(envelope.payload, static_cast<std::uint32_t>(award->players.size()));
        for (const PlayerProgressionResult& player : award->players) {
            appendUInt32(envelope.payload, player.playerId.value);
            appendUInt32(envelope.payload, player.actorId.value);
            appendInt32(envelope.payload, player.experience);
            appendInt32(envelope.payload, player.level);
            appendInt32(envelope.payload, player.unspentSkillPoints);
        }
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
    case EventType::CombatTurnStateChanged: {
        if (envelope.payload.size() < kCombatTurnEventBaseSize
            || envelope.payload[25] != 0
            || envelope.payload[26] != 0
            || envelope.payload[27] != 0) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        std::uint32_t count = readUInt32(envelope.payload, 56);
        if (count > kMaximumCombatInitiative
            || envelope.payload.size() != kCombatTurnEventBaseSize
                + static_cast<std::size_t>(count) * 8) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        CombatTurnStateChangedEvent combat;
        combat.actorId.value = readUInt32(envelope.payload, 20);
        combat.phase = static_cast<SessionPhase>(envelope.payload[24]);
        combat.phaseRevision = readUInt32(envelope.payload, 28);
        combat.state.revision = readUInt64(envelope.payload, 32);
        combat.state.round = readUInt64(envelope.payload, 40);
        combat.state.activeIndex = readUInt32(envelope.payload, 48);
        combat.state.remainingMilliseconds = readUInt32(envelope.payload, 52);
        for (std::uint32_t index = 0; index < count; index++) {
            std::size_t offset = kCombatTurnEventBaseSize + static_cast<std::size_t>(index) * 8;
            combat.state.initiative.push_back({
                EntityId { readUInt32(envelope.payload, offset) },
                PlayerId { readUInt32(envelope.payload, offset + 4) },
            });
        }
        result.event.payload = std::move(combat);
        break;
    }
    case EventType::CombatActionResolved:
        if (envelope.payload.size() != kCombatActionEventSize
            || envelope.payload[25] != 0 || envelope.payload[26] != 0
            || envelope.payload[27] != 0) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.event.payload = CombatActionResolvedEvent {
            EntityId { readUInt32(envelope.payload, 20) },
            static_cast<CombatActionKind>(envelope.payload[24]),
            EntityId { readUInt32(envelope.payload, 28) },
            readUInt64(envelope.payload, 32),
            readUInt32(envelope.payload, 40),
        };
        break;
    case EventType::PartyExperienceAwarded: {
        if (envelope.payload.size() < kPartyExperienceEventBaseSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        std::uint32_t count = readUInt32(envelope.payload, 28);
        if (count == 0 || count > kMaximumTransitionPlayers
            || envelope.payload.size() != kPartyExperienceEventBaseSize
                + static_cast<std::size_t>(count) * kPlayerProgressionSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        PartyExperienceAwardedEvent award;
        award.actorId.value = readUInt32(envelope.payload, 20);
        award.amount = readInt32(envelope.payload, 24);
        for (std::uint32_t index = 0; index < count; index++) {
            std::size_t offset = kPartyExperienceEventBaseSize
                + static_cast<std::size_t>(index) * kPlayerProgressionSize;
            award.players.push_back(PlayerProgressionResult {
                PlayerId { readUInt32(envelope.payload, offset) },
                EntityId { readUInt32(envelope.payload, offset + 4) },
                readInt32(envelope.payload, offset + 8),
                readInt32(envelope.payload, offset + 12),
                readInt32(envelope.payload, offset + 16),
            });
        }
        result.event.payload = std::move(award);
        break;
    }
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
    case EventType::WorldMapRouteSelected:
        if (envelope.payload.size() != kWorldMapRouteEventSize
            || envelope.payload[32] > 1
            || envelope.payload[33] != 0
            || envelope.payload[34] != 0
            || envelope.payload[35] != 0) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.event.payload = WorldMapRouteSelectedEvent {
            EntityId { readUInt32(envelope.payload, 20) },
            readInt32(envelope.payload, 24),
            readInt32(envelope.payload, 28),
            envelope.payload[32] != 0,
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
    case EventType::WorldMapArrived: {
        if (envelope.payload.size() < kWorldMapArrivalEventBaseSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        std::uint32_t placementCount = readUInt32(envelope.payload, 48);
        if (placementCount == 0
            || placementCount > kMaximumTransitionPlayers
            || readInt32(envelope.payload, 52) < 0
            || readInt32(envelope.payload, 52) > 32
            || readUInt32(envelope.payload, 44)
                > static_cast<std::uint32_t>(WorldMapArrivalKind::Fatal)
            || envelope.payload.size() != kWorldMapArrivalEventBaseSize
                    + static_cast<std::size_t>(placementCount) * kTransitionPlacementSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        WorldMapArrivedEvent arrival;
        arrival.actorId = EntityId { readUInt32(envelope.payload, 20) };
        arrival.map = readInt32(envelope.payload, 24);
        arrival.phaseRevision = readUInt32(envelope.payload, 28);
        arrival.worldX = readInt32(envelope.payload, 32);
        arrival.worldY = readInt32(envelope.payload, 36);
        arrival.gameTime = readInt32(envelope.payload, 40);
        arrival.kind = static_cast<WorldMapArrivalKind>(readUInt32(envelope.payload, 44));
        arrival.entranceIndex = readInt32(envelope.payload, 52);
        arrival.placements.reserve(placementCount);
        for (std::uint32_t index = 0; index < placementCount; index++) {
            std::size_t offset = kWorldMapArrivalEventBaseSize
                + static_cast<std::size_t>(index) * kTransitionPlacementSize;
            arrival.placements.push_back(PlayerTransitionPlacement {
                PlayerId { readUInt32(envelope.payload, offset) },
                EntityId { readUInt32(envelope.payload, offset + 4) },
                readInt32(envelope.payload, offset + 8),
                readInt32(envelope.payload, offset + 12),
                readInt32(envelope.payload, offset + 16),
            });
        }
        result.event.payload = std::move(arrival);
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
    case EventType::DialogueRequested:
        if (envelope.payload.size() != kTargetEventSize + 4) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.event.payload = DialogueRequestedEvent {
            EntityId { readUInt32(envelope.payload, 20) },
            EntityId { readUInt32(envelope.payload, 24) },
            readUInt32(envelope.payload, 28),
        };
        break;
    case EventType::DialogueVoteRecorded:
        if (envelope.payload.size() != kDialogueVoteEventSize
            || readUInt32(envelope.payload, 32) >= kMaximumDialogueOptions) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.event.payload = DialogueVoteRecordedEvent {
            EntityId { readUInt32(envelope.payload, 20) },
            readUInt64(envelope.payload, 24),
            static_cast<std::uint8_t>(readUInt32(envelope.payload, 32)),
        };
        break;
    case EventType::DialoguePresentation: {
        if (envelope.payload.size() < 40) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        DialoguePresentationEvent presentation;
        presentation.actorId.value = readUInt32(envelope.payload, 20);
        presentation.targetId.value = readUInt32(envelope.payload, 24);
        presentation.revision = readUInt64(envelope.payload, 28);
        presentation.policy = envelope.payload[36];
        std::uint8_t count = envelope.payload[37];
        std::uint16_t length = readUInt16(envelope.payload, 38);
        if (!isValid(presentation.actorId) || !isValid(presentation.targetId)
            || presentation.revision == 0
            || presentation.policy < 1 || presentation.policy > 4) {
            result.error = GameplayWireError::InvalidModal;
            return result;
        }
        if (count == 0 || count > kMaximumDialogueOptions || length == 0 || length > 899
            || envelope.payload.size() < 40 + length) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        std::size_t offset = 40;
        presentation.reply.assign(envelope.payload.begin() + offset,
            envelope.payload.begin() + offset + length);
        offset += length;
        for (std::uint8_t index = 0; index < count; index++) {
            if (offset + 2 > envelope.payload.size()) {
                result.error = GameplayWireError::InvalidLength;
                return result;
            }
            length = readUInt16(envelope.payload, offset);
            offset += 2;
            if (length == 0 || length > 899 || offset + length > envelope.payload.size()) {
                result.error = GameplayWireError::InvalidLength;
                return result;
            }
            presentation.options.emplace_back(envelope.payload.begin() + offset,
                envelope.payload.begin() + offset + length);
            offset += length;
        }
        if (offset != envelope.payload.size()) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.event.payload = std::move(presentation);
        break;
    }
    case EventType::SharedActivityPublished: {
        if (envelope.payload.size() < 48) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        SharedActivityPublishedEvent activity;
        activity.actorId.value = readUInt32(envelope.payload, 20);
        activity.entry.id = readUInt64(envelope.payload, 24);
        activity.entry.sourceId.value = readUInt32(envelope.payload, 32);
        activity.entry.kind = static_cast<SharedActivityKind>(envelope.payload[36]);
        std::uint8_t nameLength = envelope.payload[37];
        std::uint16_t textLength = readUInt16(envelope.payload, 38);
        activity.entry.subject = readInt32(envelope.payload, 40);
        activity.entry.value = readInt32(envelope.payload, 44);
        if (nameLength == 0 || nameLength > 32 || textLength == 0
            || textLength > 160 || envelope.payload.size()
                != 48 + nameLength + textLength) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        activity.entry.sourceName.assign(envelope.payload.begin() + 48,
            envelope.payload.begin() + 48 + nameLength);
        activity.entry.text.assign(envelope.payload.begin() + 48 + nameLength,
            envelope.payload.end());
        result.event.payload = std::move(activity);
        break;
    }
    case EventType::DirectTradeStateChanged: {
        if (envelope.payload.size() < 36 || envelope.payload[24] > 1
            || envelope.payload[25] > 1 || envelope.payload[27] != 0) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        DirectTradeStateChangedEvent trade;
        trade.actorId.value = readUInt32(envelope.payload, 20);
        trade.committed = envelope.payload[24] != 0;
        trade.cancelled = envelope.payload[25] != 0;
        std::uint8_t count = envelope.payload[26];
        trade.state.revision = readUInt64(envelope.payload, 28);
        if (count != 0 && count != 2) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        std::size_t offset = 36;
        for (std::uint8_t index = 0; index < count; index++) {
            if (offset + 12 > envelope.payload.size()) {
                result.error = GameplayWireError::InvalidLength;
                return result;
            }
            DirectTradeOffer offer;
            offer.playerId.value = readUInt32(envelope.payload, offset);
            offer.caps = readUInt32(envelope.payload, offset + 4);
            if (envelope.payload[offset + 8] > 1
                || envelope.payload[offset + 9] > kMaximumDirectTradeLines
                || readUInt16(envelope.payload, offset + 10) != 0) {
                result.error = GameplayWireError::InvalidReservedField;
                return result;
            }
            offer.confirmed = envelope.payload[offset + 8] != 0;
            std::uint8_t lineCount = envelope.payload[offset + 9];
            offset += 12;
            if (offset + static_cast<std::size_t>(lineCount) * 8 > envelope.payload.size()) {
                result.error = GameplayWireError::InvalidLength;
                return result;
            }
            for (std::uint8_t line = 0; line < lineCount; line++) {
                offer.items.push_back(DirectTradeLine {
                    EntityId { readUInt32(envelope.payload, offset) },
                    readUInt32(envelope.payload, offset + 4) });
                offset += 8;
            }
            trade.state.offers.push_back(std::move(offer));
        }
        if (offset != envelope.payload.size()) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        result.event.payload = std::move(trade);
        break;
    }
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
    case GameplayWireError::InvalidCombatTurn:
        return "invalid combat turn payload";
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
