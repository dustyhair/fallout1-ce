#include "multiplayer/command_processor.h"

#include <limits>

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
            AuthoritativeCommandResult replay = previousResult->second;
            replay.replayed = true;
            return replay;
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

    const SharedModalCommand* modal = std::get_if<SharedModalCommand>(&command.payload);
    SessionPhase requiredPhase = std::holds_alternative<AttackCommand>(command.payload)
        ? SessionPhase::Combat
        : modal != nullptr && !modal->open
        ? sharedModalPhase(modal->kind)
        : SessionPhase::Exploration;
    if (!session.isActive()
        || session.phase() != requiredPhase
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
    const FaceCommand* face = std::get_if<FaceCommand>(&command.payload);
    const InteractCommand* interact = std::get_if<InteractCommand>(&command.payload);
    const PickupCommand* pickup = std::get_if<PickupCommand>(&command.payload);
    const LootCommand* loot = std::get_if<LootCommand>(&command.payload);
    const UseSkillCommand* skill = std::get_if<UseSkillCommand>(&command.payload);
    const UseItemOnCommand* itemUse = std::get_if<UseItemOnCommand>(&command.payload);
    const ElevatorCommand* elevator = std::get_if<ElevatorCommand>(&command.payload);
    const ExitGridCommand* exitGrid = std::get_if<ExitGridCommand>(&command.payload);
    const SceneryTransitionCommand* sceneryTransition = std::get_if<SceneryTransitionCommand>(&command.payload);
    const InventoryTransferCommand* transfer = std::get_if<InventoryTransferCommand>(&command.payload);
    const ItemDropCommand* drop = std::get_if<ItemDropCommand>(&command.payload);
    const AttackCommand* attack = std::get_if<AttackCommand>(&command.payload);
    Object* target = nullptr;
    EntityId targetId;
    bool hasTarget = false;
    if (interact != nullptr) {
        targetId = interact->targetId;
        hasTarget = true;
    } else if (pickup != nullptr) {
        targetId = pickup->targetId;
        hasTarget = true;
    } else if (loot != nullptr) {
        targetId = loot->targetId;
        hasTarget = true;
    } else if (skill != nullptr) {
        if (!isValid(skill->skill)) {
            return rejectAndRemember(CommandRejection::Malformed);
        }
        targetId = skill->targetId;
        hasTarget = true;
    } else if (itemUse != nullptr) {
        targetId = itemUse->targetId;
        hasTarget = true;
    } else if (elevator != nullptr) {
        if (elevator->elevatorType < 0
            || elevator->elevatorType >= 12
            || elevator->destinationLevel < 0
            || elevator->destinationLevel > 3) {
            return rejectAndRemember(CommandRejection::Malformed);
        }
    } else if (exitGrid != nullptr) {
        if (!isValid(exitGrid->exitId)) {
            return rejectAndRemember(CommandRejection::Malformed);
        }
        targetId = exitGrid->exitId;
        hasTarget = true;
    } else if (sceneryTransition != nullptr) {
        if (!isValid(sceneryTransition->transitionId)) {
            return rejectAndRemember(CommandRejection::Malformed);
        }
        targetId = sceneryTransition->transitionId;
        hasTarget = true;
    } else if (attack != nullptr) {
        targetId = attack->targetId;
        hasTarget = true;
    }
    if (hasTarget) {
        target = session.entities().findObject(targetId);
        if (target == nullptr) {
            return rejectAndRemember(CommandRejection::MissingEntity);
        }
    }

    Object* source = nullptr;
    Object* destination = nullptr;
    Object* item = nullptr;
    if (itemUse != nullptr) {
        if (!isValid(itemUse->itemId) || itemUse->itemId == itemUse->targetId) {
            return rejectAndRemember(CommandRejection::Malformed);
        }
        item = session.entities().findObject(itemUse->itemId);
        if (item == nullptr) {
            return rejectAndRemember(CommandRejection::MissingEntity);
        }
    } else if (transfer != nullptr) {
        bool descriptorValid = hasItemDescriptor(transfer->itemDescriptor)
            && transfer->itemDescriptor.pid >= 0
            && (static_cast<std::uint32_t>(transfer->itemDescriptor.pid) >> 24) == 0;
        if (transfer->quantity == 0
            || transfer->quantity > transfer->sourceQuantity
            || transfer->sourceQuantity > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
            || (!isValid(transfer->itemId) && !descriptorValid)) {
            return rejectAndRemember(CommandRejection::Malformed);
        }
        source = session.entities().findObject(transfer->sourceId);
        destination = session.entities().findObject(transfer->destinationId);
        item = isValid(transfer->itemId) ? session.entities().findObject(transfer->itemId) : nullptr;
        if (source == nullptr
            || destination == nullptr
            || (isValid(transfer->itemId) && item == nullptr)) {
            return rejectAndRemember(CommandRejection::MissingEntity);
        }
    } else if (drop != nullptr) {
        bool descriptorValid = hasItemDescriptor(drop->itemDescriptor)
            && drop->itemDescriptor.pid >= 0
            && (static_cast<std::uint32_t>(drop->itemDescriptor.pid) >> 24) == 0;
        if (drop->quantity == 0
            || drop->quantity > drop->sourceQuantity
            || drop->sourceQuantity > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
            || (!isValid(drop->itemId) && !descriptorValid)) {
            return rejectAndRemember(CommandRejection::Malformed);
        }
        source = session.entities().findObject(drop->sourceId);
        item = isValid(drop->itemId) ? session.entities().findObject(drop->itemId) : nullptr;
        if (source == nullptr || (isValid(drop->itemId) && item == nullptr)) {
            return rejectAndRemember(CommandRejection::MissingEntity);
        }
    }

    CommandExecutionStatus executionStatus = CommandExecutionStatus::InvalidAction;
    {
        ScopedActingPlayerContext actingPlayer(*actingState, actor);
        if (move != nullptr) {
            executionStatus = executor.move(actor, *move);
            event.payload = ActorMovementStartedEvent { command.actorId, move->destinationTile, move->elevation, move->running };
        } else if (face != nullptr) {
            executionStatus = executor.face(actor, *face);
            event.payload = ActorFacingChangedEvent { command.actorId, face->rotation };
        } else if (interact != nullptr) {
            DoorUseExecution doorExecution = executor.useDoor(actor, target);
            executionStatus = doorExecution.status;
            event.payload = DoorUseStartedEvent {
                command.actorId,
                interact->targetId,
                doorExecution.open,
                doorExecution.locked,
                doorExecution.frame,
            };
        } else if (pickup != nullptr) {
            executionStatus = executor.pickup(actor, target);
            event.payload = ItemPickupStartedEvent { command.actorId, pickup->targetId };
        } else if (loot != nullptr) {
            executionStatus = executor.loot(actor, target);
            event.payload = LootStartedEvent { command.actorId, loot->targetId };
        } else if (skill != nullptr) {
            executionStatus = executor.useSkill(actor, target, *skill);
            event.payload = SkillUseStartedEvent { command.actorId, skill->targetId, skill->skill };
        } else if (itemUse != nullptr) {
            executionStatus = executor.useItemOn(actor, item, target, *itemUse);
            event.payload = ItemUseStartedEvent { command.actorId, itemUse->itemId, itemUse->targetId };
        } else if (elevator != nullptr) {
            ElevatorExecution elevatorExecution = executor.useElevator(actor, *elevator);
            executionStatus = elevatorExecution.status;
            event.payload = ElevatorTransitionedEvent {
                command.actorId,
                elevator->elevatorType,
                elevatorExecution.map,
                elevatorExecution.hostTile,
                elevatorExecution.hostElevation,
                elevatorExecution.hostRotation,
                elevatorExecution.guestTile,
                elevatorExecution.guestElevation,
                elevatorExecution.guestRotation,
                elevatorExecution.phaseRevision,
            };
        } else if (exitGrid != nullptr) {
            ExitGridExecution exitExecution = executor.useExitGrid(actor, target, *exitGrid);
            executionStatus = exitExecution.status;
            event.payload = ExitGridTransitionedEvent {
                command.actorId,
                exitGrid->exitId,
                exitExecution.map,
                std::move(exitExecution.placements),
                exitExecution.phaseRevision,
            };
        } else if (sceneryTransition != nullptr) {
            SceneryTransitionExecution transitionExecution = executor.useSceneryTransition(actor, target, *sceneryTransition);
            executionStatus = transitionExecution.status;
            event.payload = SceneryTransitionedEvent {
                command.actorId,
                sceneryTransition->transitionId,
                transitionExecution.map,
                std::move(transitionExecution.placements),
                transitionExecution.phaseRevision,
            };
        } else if (attack != nullptr) {
            executionStatus = executor.attack(actor, target, *attack);
            event.payload = AttackStartedEvent { command.actorId, attack->targetId, attack->hitMode, attack->hitLocation };
        } else if (modal != nullptr) {
            SharedModalExecution modalExecution = executor.setSharedModal(actor, *modal);
            executionStatus = modalExecution.status;
            event.payload = SharedModalStateChangedEvent {
                command.actorId,
                modal->kind,
                modal->open,
                modalExecution.phase,
                modalExecution.phaseRevision,
            };
        } else if (transfer != nullptr) {
            InventoryTransferExecution transferExecution = executor.transferInventory(actor,
                source,
                destination,
                item,
                *transfer);
            executionStatus = transferExecution.status;
            event.payload = InventoryTransferredEvent {
                command.actorId,
                transfer->sourceId,
                transfer->destinationId,
                transferExecution.itemId,
                transfer->quantity,
                transfer->sourceQuantity,
                transferExecution.remainderItemId,
                transferExecution.itemDescriptor,
            };
        } else if (drop != nullptr) {
            ItemDropExecution dropExecution = executor.dropItem(actor,
                source,
                item,
                *drop);
            executionStatus = dropExecution.status;
            event.payload = ItemDroppedEvent {
                command.actorId,
                drop->sourceId,
                dropExecution.itemId,
                drop->quantity,
                drop->sourceQuantity,
                dropExecution.remainderItemId,
                dropExecution.tile,
                dropExecution.elevation,
                dropExecution.itemDescriptor,
            };
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
