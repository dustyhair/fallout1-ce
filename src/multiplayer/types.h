#ifndef FALLOUT_MULTIPLAYER_TYPES_H_
#define FALLOUT_MULTIPLAYER_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

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

enum class SharedModalKind : std::uint8_t {
    Dialogue = 1,
    Barter = 2,
    Rest = 3,
    Elevator = 4,
    WorldMap = 5,
};

constexpr bool isValid(SharedModalKind kind)
{
    return kind >= SharedModalKind::Dialogue && kind <= SharedModalKind::WorldMap;
}

constexpr SessionPhase sharedModalPhase(SharedModalKind kind)
{
    return kind == SharedModalKind::Dialogue || kind == SharedModalKind::Barter
        ? SessionPhase::Dialogue
        : SessionPhase::Transition;
}

struct MoveCommand {
    std::int32_t destinationTile = -1;
    std::int32_t elevation = -1;
    bool running = false;
};

struct FaceCommand {
    std::int32_t rotation = 0;
};

struct InteractCommand {
    EntityId targetId;
};

struct PickupCommand {
    EntityId targetId;
};

struct LootCommand {
    EntityId targetId;
};

// These values intentionally match Fallout's Skill enum. Only targeted,
// exploration-safe skills belong in the multiplayer command surface.
enum class ExplorationSkill : std::int32_t {
    FirstAid = 6,
    Doctor = 7,
    Lockpick = 9,
    Steal = 10,
    Traps = 11,
    Science = 12,
    Repair = 13,
};

constexpr bool isValid(ExplorationSkill skill)
{
    switch (skill) {
    case ExplorationSkill::FirstAid:
    case ExplorationSkill::Doctor:
    case ExplorationSkill::Lockpick:
    case ExplorationSkill::Steal:
    case ExplorationSkill::Traps:
    case ExplorationSkill::Science:
    case ExplorationSkill::Repair:
        return true;
    }
    return false;
}

struct UseSkillCommand {
    EntityId targetId;
    ExplorationSkill skill = ExplorationSkill::FirstAid;
};

struct UseItemOnCommand {
    EntityId itemId;
    EntityId targetId;
};

struct ElevatorCommand {
    std::int32_t elevatorType = -1;
    std::int32_t destinationLevel = -1;
};

struct ExitGridCommand {
    EntityId exitId;
};

struct SceneryTransitionCommand {
    EntityId transitionId;
};

// Positive values are fixed minutes; zero withdraws consent. Negative values
// preserve the Pip-Boy choice until the host resolves it at unanimous consent.
constexpr std::int32_t kRestUntilMorning = -1;
constexpr std::int32_t kRestUntilNoon = -2;
constexpr std::int32_t kRestUntilEvening = -3;
constexpr std::int32_t kRestUntilMidnight = -4;
constexpr std::int32_t kRestUntilHealed = -5;
struct RestCommand {
    std::int32_t minutes = 0;
};

constexpr bool isValidRestMinutes(std::int32_t minutes)
{
    return (minutes >= kRestUntilHealed && minutes <= 0)
        || minutes == 10 || minutes == 30
        || (minutes >= 60 && minutes <= 360 && minutes % 60 == 0);
}

// Returns 1..1440 minutes. Selecting the current hour means the next day.
constexpr int restMinutesUntilHour(std::int32_t choice, int gameHour)
{
    int target = choice == kRestUntilMorning ? 6
        : choice == kRestUntilNoon ? 12
        : choice == kRestUntilEvening ? 18
        : choice == kRestUntilMidnight ? 0
        : -1;
    int currentHour = gameHour / 100;
    int currentMinute = gameHour % 100;
    if (target < 0 || currentHour < 0 || currentHour >= 24
        || currentMinute < 0 || currentMinute >= 60) {
        return 0;
    }
    int minutes = (target * 60 - currentHour * 60 - currentMinute + 1440) % 1440;
    return minutes == 0 ? 1440 : minutes;
}

constexpr const char* restChoiceName(std::int32_t choice)
{
    return choice == kRestUntilMorning ? "until_morning"
        : choice == kRestUntilNoon ? "until_noon"
        : choice == kRestUntilEvening ? "until_evening"
        : choice == kRestUntilMidnight ? "until_midnight"
        : choice == kRestUntilHealed ? "until_healed"
        : "fixed";
}

struct ItemDescriptor {
    std::int32_t pid = -1;
    std::int32_t extendedFlags = 0;
    std::int32_t data0 = 0;
    std::int32_t data1 = 0;
};

constexpr bool hasItemDescriptor(const ItemDescriptor& descriptor)
{
    return descriptor.pid != -1;
}

struct InventoryTransferCommand {
    EntityId sourceId;
    EntityId destinationId;
    EntityId itemId;
    std::uint32_t quantity = 0;
    std::uint32_t sourceQuantity = 0;
    ItemDescriptor itemDescriptor;
};

struct ItemDropCommand {
    EntityId sourceId;
    EntityId itemId;
    std::uint32_t quantity = 0;
    std::uint32_t sourceQuantity = 0;
    ItemDescriptor itemDescriptor;
};

struct AttackCommand {
    EntityId targetId;
    std::int32_t hitMode = 0;
    std::int32_t hitLocation = 0;
};

struct SharedModalCommand {
    SharedModalKind kind = SharedModalKind::Dialogue;
    bool open = false;
};

using GameCommandPayload = std::variant<MoveCommand, FaceCommand, InteractCommand, PickupCommand, LootCommand, UseSkillCommand, UseItemOnCommand, ElevatorCommand, ExitGridCommand, SceneryTransitionCommand, RestCommand, InventoryTransferCommand, ItemDropCommand, AttackCommand, SharedModalCommand>;

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
    std::int32_t startingTile = -1;
    std::vector<std::uint8_t> path;
};

constexpr std::size_t kMaximumMovementPathLength = 800;

struct ActorFacingChangedEvent {
    EntityId actorId;
    std::int32_t rotation = 0;
};

constexpr std::int32_t kActorRotationCount = 6;

struct DoorUseStartedEvent {
    EntityId actorId;
    EntityId targetId;
    bool open = false;
    bool locked = false;
    std::int32_t frame = 0;
};

struct ItemPickupStartedEvent {
    EntityId actorId;
    EntityId targetId;
};

struct ItemPickupCompletedEvent {
    EntityId actorId;
    EntityId targetId;
    bool succeeded = false;
    std::uint32_t quantity = 0;
    ItemDescriptor itemDescriptor;
};

struct LootStartedEvent {
    EntityId actorId;
    EntityId targetId;
};

struct SkillUseStartedEvent {
    EntityId actorId;
    EntityId targetId;
    ExplorationSkill skill = ExplorationSkill::FirstAid;
};

struct ItemUseStartedEvent {
    EntityId actorId;
    EntityId itemId;
    EntityId targetId;
};

struct ElevatorTransitionedEvent {
    EntityId actorId;
    std::int32_t elevatorType = -1;
    std::int32_t map = -1;
    std::int32_t hostTile = -1;
    std::int32_t hostElevation = -1;
    std::int32_t hostRotation = 0;
    std::int32_t guestTile = -1;
    std::int32_t guestElevation = -1;
    std::int32_t guestRotation = 0;
    std::uint32_t phaseRevision = 0;
};

constexpr std::size_t kMaximumTransitionPlayers = 16;

struct PlayerTransitionPlacement {
    PlayerId playerId;
    EntityId actorId;
    std::int32_t tile = -1;
    std::int32_t elevation = -1;
    std::int32_t rotation = 0;
};

struct ExitGridTransitionedEvent {
    EntityId actorId;
    EntityId exitId;
    std::int32_t map = -1;
    std::vector<PlayerTransitionPlacement> placements;
    std::uint32_t phaseRevision = 0;
};

struct SceneryTransitionedEvent {
    EntityId actorId;
    EntityId transitionId;
    std::int32_t map = -1;
    std::vector<PlayerTransitionPlacement> placements;
    std::uint32_t phaseRevision = 0;
};

struct RestStateChangedEvent {
    EntityId actorId;
    std::int32_t minutes = 0;
    bool completed = false;
    std::int32_t gameTime = 0;
    std::uint32_t phaseRevision = 0;
    bool interrupted = false;
};

struct InventoryTransferredEvent {
    EntityId actorId;
    EntityId sourceId;
    EntityId destinationId;
    EntityId itemId;
    std::uint32_t quantity = 0;
    std::uint32_t sourceQuantity = 0;
    EntityId remainderItemId;
    ItemDescriptor itemDescriptor;
};

struct ItemDroppedEvent {
    EntityId actorId;
    EntityId sourceId;
    EntityId itemId;
    std::uint32_t quantity = 0;
    std::uint32_t sourceQuantity = 0;
    EntityId remainderItemId;
    std::int32_t tile = -1;
    std::int32_t elevation = -1;
    ItemDescriptor itemDescriptor;
};

struct AttackStartedEvent {
    EntityId actorId;
    EntityId targetId;
    std::int32_t hitMode = 0;
    std::int32_t hitLocation = 0;
};

struct SharedModalStateChangedEvent {
    EntityId actorId;
    SharedModalKind kind = SharedModalKind::Dialogue;
    bool open = false;
    SessionPhase phase = SessionPhase::Exploration;
    std::uint32_t phaseRevision = 0;
};

using GameEventPayload = std::variant<ActorMovementStartedEvent, ActorFacingChangedEvent, DoorUseStartedEvent, ItemPickupStartedEvent, ItemPickupCompletedEvent, LootStartedEvent, SkillUseStartedEvent, ItemUseStartedEvent, ElevatorTransitionedEvent, ExitGridTransitionedEvent, SceneryTransitionedEvent, RestStateChangedEvent, InventoryTransferredEvent, ItemDroppedEvent, AttackStartedEvent, SharedModalStateChangedEvent>;

struct GameEvent {
    EventSequence sequence;
    CommandSequence causedBy;
    GameEventPayload payload;
};

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_TYPES_H_ */
