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

class CommandExecutor {
public:
    virtual ~CommandExecutor() = default;

    virtual CommandExecutionStatus move(Object* actor, const MoveCommand& command) = 0;
    virtual CommandExecutionStatus face(Object* actor, const FaceCommand& command) = 0;
    virtual CommandExecutionStatus useDoor(Object* actor, Object* target) = 0;
    virtual CommandExecutionStatus pickup(Object* actor, Object* target) = 0;
    virtual CommandExecutionStatus loot(Object* actor, Object* target) = 0;
};

struct AuthoritativeCommandResult {
    CommandResult result;
    std::optional<GameEvent> event;
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
