#include "multiplayer/command_processor.h"

#include "multiplayer/acting_player_context.h"

namespace fallout {
namespace multiplayer {

AuthoritativeCommandResult CommandProcessor::process(const GameCommand& command, LocalSession& session, CommandExecutor& executor)
{
    if (!isValid(command.playerId)
        || (command.playerId != kHostPlayerId && command.playerId != kGuestPlayerId)
        || !isValid(command.actorId)
        || command.sequence.value == 0) {
        return reject(command, CommandRejection::Malformed);
    }

    auto commandHistory = _players.find(command.playerId);
    std::uint64_t expectedSequence = 1;
    if (commandHistory != _players.end()) {
        auto previousResult = commandHistory->second.results.find(command.sequence.value);
        if (previousResult != commandHistory->second.results.end()) {
            return previousResult->second;
        }
        expectedSequence = commandHistory->second.lastSequence + 1;
    }

    if (command.sequence.value != expectedSequence) {
        return reject(command, CommandRejection::Stale);
    }

    auto rejectAndRemember = [&](CommandRejection rejection) {
        AuthoritativeCommandResult result = reject(command, rejection);
        remember(command.playerId, command.sequence.value, result);
        return result;
    };

    if (!session.isActive()
        || session.phase() != SessionPhase::Exploration
        || command.expectedPhase != session.phase()) {
        return rejectAndRemember(CommandRejection::WrongPhase);
    }

    if (command.expectedPhaseRevision != session.phaseRevision()) {
        return rejectAndRemember(CommandRejection::Stale);
    }

    Object* actor = session.entities().findObject(command.actorId);
    if (actor == nullptr) {
        return rejectAndRemember(CommandRejection::MissingEntity);
    }

    if (!session.owns(command.playerId, command.actorId)) {
        return rejectAndRemember(CommandRejection::NotOwner);
    }

    PlayerCharacterState* actingState = session.players().find(command.playerId);
    if (actingState == nullptr || actingState->actorId != command.actorId) {
        return rejectAndRemember(CommandRejection::NotOwner);
    }

    GameEvent event;
    event.sequence.value = _nextEventSequence;
    event.causedBy = command.sequence;

    const MoveCommand* move = std::get_if<MoveCommand>(&command.payload);
    const InteractCommand* interact = std::get_if<InteractCommand>(&command.payload);
    Object* target = nullptr;
    if (interact != nullptr) {
        target = session.entities().findObject(interact->targetId);
        if (target == nullptr) {
            return rejectAndRemember(CommandRejection::MissingEntity);
        }
    }

    CommandExecutionStatus executionStatus = CommandExecutionStatus::InvalidAction;
    {
        ScopedActingPlayerContext actingPlayer(*actingState, actor);
        if (move != nullptr) {
            executionStatus = executor.move(actor, *move);
            event.payload = ActorMovementStartedEvent { command.actorId, move->destinationTile, move->elevation, move->running };
        } else if (interact != nullptr) {
            executionStatus = executor.useDoor(actor, target);
            event.payload = DoorUseStartedEvent { command.actorId, interact->targetId };
        }
    }

    if (executionStatus != CommandExecutionStatus::Applied) {
        return rejectAndRemember(CommandRejection::InvalidAction);
    }

    AuthoritativeCommandResult authoritative;
    authoritative.result.commandSequence = command.sequence;
    authoritative.result.status = CommandStatus::Accepted;
    authoritative.result.rejection = CommandRejection::None;
    authoritative.result.firstEventSequence = event.sequence;
    authoritative.result.eventCount = 1;
    authoritative.event = event;

    _nextEventSequence++;
    remember(command.playerId, command.sequence.value, authoritative);
    return authoritative;
}

void CommandProcessor::reset()
{
    _players.clear();
    _nextEventSequence = 1;
}

AuthoritativeCommandResult CommandProcessor::reject(const GameCommand& command, CommandRejection rejection)
{
    AuthoritativeCommandResult authoritative;
    authoritative.result.commandSequence = command.sequence;
    authoritative.result.status = CommandStatus::Rejected;
    authoritative.result.rejection = rejection;
    return authoritative;
}

void CommandProcessor::remember(PlayerId playerId, std::uint64_t sequence, const AuthoritativeCommandResult& result)
{
    PlayerCommandState& player = _players[playerId];
    player.lastSequence = sequence;
    player.resultOrder.push_back(sequence);
    player.results.emplace(sequence, result);

    if (player.resultOrder.size() > kResultHistorySize) {
        player.results.erase(player.resultOrder.front());
        player.resultOrder.pop_front();
    }
}

} // namespace multiplayer
} // namespace fallout
