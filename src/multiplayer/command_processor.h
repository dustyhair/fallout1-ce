#ifndef FALLOUT_MULTIPLAYER_COMMAND_PROCESSOR_H_
#define FALLOUT_MULTIPLAYER_COMMAND_PROCESSOR_H_

#include <deque>
#include <optional>
#include <unordered_map>

#include "multiplayer/local_session.h"
#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

enum class CommandExecutionStatus {
    Applied,
    InvalidAction,
};

struct InventoryTransferExecution {
    CommandExecutionStatus status = CommandExecutionStatus::InvalidAction;
    EntityId itemId;
    EntityId remainderItemId;
    ItemDescriptor itemDescriptor;
};

struct DoorUseExecution {
    CommandExecutionStatus status = CommandExecutionStatus::InvalidAction;
    bool open = false;
    bool locked = false;
    std::int32_t frame = 0;
};

struct ItemDropExecution {
    CommandExecutionStatus status = CommandExecutionStatus::InvalidAction;
    EntityId itemId;
    EntityId remainderItemId;
    std::int32_t tile = -1;
    std::int32_t elevation = -1;
    ItemDescriptor itemDescriptor;
};

struct SharedModalExecution {
    CommandExecutionStatus status = CommandExecutionStatus::InvalidAction;
    SessionPhase phase = SessionPhase::Exploration;
    std::uint32_t phaseRevision = 0;
    EntityId actorId;
};

struct ElevatorExecution {
    CommandExecutionStatus status = CommandExecutionStatus::InvalidAction;
    std::int32_t map = -1;
    std::int32_t hostTile = -1;
    std::int32_t hostElevation = -1;
    std::int32_t hostRotation = 0;
    std::int32_t guestTile = -1;
    std::int32_t guestElevation = -1;
    std::int32_t guestRotation = 0;
    std::uint32_t phaseRevision = 0;
};

struct ExitGridExecution {
    CommandExecutionStatus status = CommandExecutionStatus::InvalidAction;
    std::int32_t map = -1;
    std::vector<PlayerTransitionPlacement> placements;
    std::uint32_t phaseRevision = 0;
};

using SceneryTransitionExecution = ExitGridExecution;

struct RestExecution {
    CommandExecutionStatus status = CommandExecutionStatus::InvalidAction;
    bool completed = false;
    std::int32_t gameTime = 0;
    std::uint32_t phaseRevision = 0;
    bool interrupted = false;
};

class CommandExecutor {
public:
    virtual ~CommandExecutor() = default;

    virtual CommandExecutionStatus move(Object* actor, const MoveCommand& command) = 0;
    virtual CommandExecutionStatus face(Object* actor, const FaceCommand& command) = 0;
    virtual DoorUseExecution useDoor(Object* actor, Object* target) = 0;
    virtual CommandExecutionStatus pickup(Object* actor, Object* target) = 0;
    virtual CommandExecutionStatus loot(Object* actor, Object* target) = 0;
    virtual CommandExecutionStatus useSkill(Object*, Object*, const UseSkillCommand&)
    {
        return CommandExecutionStatus::InvalidAction;
    }
    virtual CommandExecutionStatus useItemOn(Object*, Object*, Object*, const UseItemOnCommand&)
    {
        return CommandExecutionStatus::InvalidAction;
    }
    virtual ElevatorExecution useElevator(Object*, const ElevatorCommand&)
    {
        return {};
    }
    virtual ExitGridExecution useExitGrid(Object*, Object*, const ExitGridCommand&)
    {
        return {};
    }
    virtual SceneryTransitionExecution useSceneryTransition(Object*, Object*, const SceneryTransitionCommand&)
    {
        return {};
    }
    virtual RestExecution rest(Object*, const RestCommand&)
    {
        return {};
    }
    virtual CommandExecutionStatus attack(Object*, Object*, const AttackCommand&)
    {
        return CommandExecutionStatus::InvalidAction;
    }
    virtual SharedModalExecution setSharedModal(Object*, const SharedModalCommand&)
    {
        return {};
    }
    virtual CommandExecutionStatus setWorldMapRoute(Object*, const WorldMapRouteCommand&)
    {
        return CommandExecutionStatus::InvalidAction;
    }
    virtual InventoryTransferExecution transferInventory(Object* actor,
        Object* source,
        Object* destination,
        Object* item,
        const InventoryTransferCommand& command) = 0;
    virtual ItemDropExecution dropItem(Object* actor,
        Object* source,
        Object* item,
        const ItemDropCommand& command) = 0;
};

struct AuthoritativeCommandResult {
    CommandResult result;
    std::optional<GameEvent> event;
    bool replayed = false;
};

class CommandProcessor {
public:
    AuthoritativeCommandResult process(const GameCommand& command, LocalSession& session, CommandExecutor& executor);
    void reset();

private:
    static constexpr std::size_t kResultHistorySize = 64;

    struct PlayerCommandState {
        std::uint64_t lastSequence = 0;
        std::deque<std::uint64_t> resultOrder;
        std::unordered_map<std::uint64_t, AuthoritativeCommandResult> results;
    };

    AuthoritativeCommandResult reject(const GameCommand& command, CommandRejection rejection);
    void remember(PlayerId playerId, std::uint64_t sequence, const AuthoritativeCommandResult& result);

    std::unordered_map<PlayerId, PlayerCommandState, PlayerIdHash> _players;
    std::uint64_t _nextEventSequence = 1;
};

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_COMMAND_PROCESSOR_H_ */
