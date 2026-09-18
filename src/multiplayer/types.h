#ifndef FALLOUT_MULTIPLAYER_TYPES_H_
#define FALLOUT_MULTIPLAYER_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <variant>

namespace fallout {
namespace multiplayer {

struct PlayerId {
    std::uint32_t value = 0;
};

constexpr PlayerId kHostPlayerId { 1 };
constexpr PlayerId kGuestPlayerId { 2 };

constexpr bool isValid(PlayerId id)
{
    return id.value != 0;
}

constexpr bool operator==(PlayerId lhs, PlayerId rhs)
{
    return lhs.value == rhs.value;
}

constexpr bool operator!=(PlayerId lhs, PlayerId rhs)
{
    return !(lhs == rhs);
}

struct PlayerIdHash {
    std::size_t operator()(PlayerId id) const
    {
        return static_cast<std::size_t>(id.value);
    }
};

struct EntityId {
    std::uint32_t value = 0;
};

constexpr bool isValid(EntityId id)
{
    return id.value != 0;
}

constexpr bool operator==(EntityId lhs, EntityId rhs)
{
    return lhs.value == rhs.value;
}

constexpr bool operator!=(EntityId lhs, EntityId rhs)
{
    return !(lhs == rhs);
}

struct EntityIdHash {
    std::size_t operator()(EntityId id) const
    {
        return static_cast<std::size_t>(id.value);
    }
};

struct SessionId {
    std::uint64_t value = 0;
};

constexpr bool operator==(SessionId lhs, SessionId rhs)
{
    return lhs.value == rhs.value;
}

constexpr bool operator!=(SessionId lhs, SessionId rhs)
{
    return !(lhs == rhs);
}

struct CommandSequence {
    std::uint64_t value = 0;
};

constexpr bool operator==(CommandSequence lhs, CommandSequence rhs)
{
    return lhs.value == rhs.value;
}

constexpr bool operator!=(CommandSequence lhs, CommandSequence rhs)
{
    return !(lhs == rhs);
}

struct EventSequence {
    std::uint64_t value = 0;
};

constexpr bool operator==(EventSequence lhs, EventSequence rhs)
{
    return lhs.value == rhs.value;
}

constexpr bool operator!=(EventSequence lhs, EventSequence rhs)
{
    return !(lhs == rhs);
}

enum class SessionPhase : std::uint8_t {
    Lobby = 1,
    Loading = 2,
    Exploration = 3,
    Combat = 4,
    Dialogue = 5,
    Transition = 6,
    Ending = 7,
};

struct MoveCommand {
    std::int32_t destinationTile = -1;
    std::int32_t elevation = -1;
    bool running = false;
};

struct InteractCommand {
    EntityId targetId;
};

using GameCommandPayload = std::variant<MoveCommand, InteractCommand>;

struct GameCommand {
    CommandSequence sequence;
    PlayerId playerId;
    EntityId actorId;
    SessionPhase expectedPhase = SessionPhase::Lobby;
    std::uint32_t expectedPhaseRevision = 0;
    GameCommandPayload payload;
};

enum class CommandStatus : std::uint8_t {
    Accepted = 1,
    Rejected = 2,
};

enum class CommandRejection : std::uint8_t {
    None = 0,
    Malformed = 1,
    WrongPhase = 2,
    Stale = 3,
    NotOwner = 4,
    MissingEntity = 5,
    InvalidAction = 6,
};

struct CommandResult {
    CommandSequence commandSequence;
    CommandStatus status = CommandStatus::Rejected;
    CommandRejection rejection = CommandRejection::Malformed;
    EventSequence firstEventSequence;
    std::uint32_t eventCount = 0;
};

struct ActorMovementStartedEvent {
    EntityId actorId;
    std::int32_t destinationTile = -1;
    std::int32_t elevation = -1;
    bool running = false;
};

struct DoorUseStartedEvent {
    EntityId actorId;
    EntityId targetId;
};

using GameEventPayload = std::variant<ActorMovementStartedEvent, DoorUseStartedEvent>;

struct GameEvent {
    EventSequence sequence;
    CommandSequence causedBy;
    GameEventPayload payload;
};

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_TYPES_H_ */
