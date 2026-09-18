#include "multiplayer/gameplay_wire.h"

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
};

enum class EventType : std::uint8_t {
    ActorMovementStarted = 1,
    DoorUseStarted = 2,
    ItemPickupStarted = 3,
    LootStarted = 4,
    ActorFacingChanged = 5,
};

constexpr std::size_t kCommandHeaderSize = 28;
constexpr std::size_t kMoveCommandSize = kCommandHeaderSize + 12;
constexpr std::size_t kTargetCommandSize = kCommandHeaderSize + 4;
constexpr std::size_t kFacingCommandSize = kCommandHeaderSize + 8;
constexpr std::size_t kCommandResultSize = 24;
constexpr std::size_t kEventHeaderSize = 20;
constexpr std::size_t kMovementEventHeaderSize = kEventHeaderSize + 20;
constexpr std::size_t kTargetEventSize = kEventHeaderSize + 8;
constexpr std::size_t kFacingEventSize = kEventHeaderSize + 8;

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

    EntityId actorId;
    EntityId targetId;
    if (const auto* door = std::get_if<DoorUseStartedEvent>(&event.payload)) {
        actorId = door->actorId;
        targetId = door->targetId;
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
    } else {
        const auto* loot = std::get_if<LootCommand>(&command.payload);
        appendCommandHeader(command, CommandType::Loot, envelope.payload);
        appendUInt32(envelope.payload, loot->targetId.value);
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
    } else if (const auto* pickup = std::get_if<ItemPickupStartedEvent>(&event.payload)) {
        appendEventHeader(event, EventType::ItemPickupStarted, envelope.payload);
        appendUInt32(envelope.payload, pickup->actorId.value);
        appendUInt32(envelope.payload, pickup->targetId.value);
    } else {
        const auto* loot = std::get_if<LootStartedEvent>(&event.payload);
        appendEventHeader(event, EventType::LootStarted, envelope.payload);
        appendUInt32(envelope.payload, loot->actorId.value);
        appendUInt32(envelope.payload, loot->targetId.value);
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
    case EventType::DoorUseStarted:
    case EventType::ItemPickupStarted:
    case EventType::LootStarted: {
        if (envelope.payload.size() != kTargetEventSize) {
            result.error = GameplayWireError::InvalidLength;
            return result;
        }
        EntityId actorId { readUInt32(envelope.payload, 20) };
        EntityId targetId { readUInt32(envelope.payload, 24) };
        if (type == EventType::DoorUseStarted) {
            result.event.payload = DoorUseStartedEvent { actorId, targetId };
        } else if (type == EventType::ItemPickupStarted) {
            result.event.payload = ItemPickupStartedEvent { actorId, targetId };
        } else {
            result.event.payload = LootStartedEvent { actorId, targetId };
        }
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
