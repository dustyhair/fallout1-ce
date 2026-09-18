#include "multiplayer/developer_local_session.h"

#include <cstring>

#include "game/actions.h"
#include "game/anim.h"
#include "game/combat.h"
#include "game/map_defs.h"
#include "game/object.h"
#include "game/protinst.h"
#include "multiplayer/character_build_bridge.h"
#include "multiplayer/command_processor.h"
#include "multiplayer/local_session.h"
#include "plib/gnw/debug.h"

namespace fallout {
namespace multiplayer {
namespace {

bool enabled = false;
Object* guestActor = nullptr;
LocalSession session;
CommandProcessor commandProcessor;
std::uint64_t nextHostCommandSequence = 1;
std::uint64_t nextGuestCommandSequence = 1;

class EngineCommandExecutor : public CommandExecutor {
public:
    CommandExecutionStatus move(Object* actor, const MoveCommand& command) override
    {
        if (isInCombat()
            || !hexGridTileIsValid(command.destinationTile)
            || !elevationIsValid(command.elevation)
            || command.elevation != actor->elevation
            || command.destinationTile == actor->tile
            || make_path(actor, actor->tile, command.destinationTile, nullptr, 1) == 0) {
            return CommandExecutionStatus::InvalidAction;
        }

        int requestOptions = actor == obj_dude
            ? ANIMATION_REQUEST_RESERVED
            : ANIMATION_REQUEST_UNRESERVED;
        if (register_begin(requestOptions) == -1) {
            return CommandExecutionStatus::InvalidAction;
        }

        int rc = command.running
            ? register_object_run_to_tile(actor, command.destinationTile, command.elevation, -1, 0)
            : register_object_move_to_tile(actor, command.destinationTile, command.elevation, -1, 0);
        if (rc == -1 || register_end() == -1) {
            return CommandExecutionStatus::InvalidAction;
        }

        return CommandExecutionStatus::Applied;
    }

    CommandExecutionStatus useDoor(Object* actor, Object* target) override
    {
        if (isInCombat()
            || actor->elevation != target->elevation
            || !obj_is_a_portal(target)
            || action_use_an_object(actor, target) == -1) {
            return CommandExecutionStatus::InvalidAction;
        }

        return CommandExecutionStatus::Applied;
    }
};

EngineCommandExecutor commandExecutor;

std::uint64_t* nextCommandSequence(PlayerId playerId)
{
    if (playerId == kHostPlayerId) {
        return &nextHostCommandSequence;
    }
    if (playerId == kGuestPlayerId) {
        return &nextGuestCommandSequence;
    }
    return nullptr;
}

bool submitCommand(PlayerId playerId, GameCommandPayload payload)
{
    std::uint64_t* nextSequence = nextCommandSequence(playerId);
    EntityId actorId = session.playerActorId(playerId);
    if (!enabled || !session.isActive() || nextSequence == nullptr || !isValid(actorId)) {
        return false;
    }

    GameCommand command;
    command.sequence.value = (*nextSequence)++;
    command.playerId = playerId;
    command.actorId = actorId;
    command.expectedPhase = session.phase();
    command.expectedPhaseRevision = session.phaseRevision();
    command.payload = payload;

    AuthoritativeCommandResult authoritative = commandProcessor.process(command, session, commandExecutor);
    if (authoritative.result.status == CommandStatus::Rejected) {
        debug_printf("Multiplayer command %llu rejected with reason %d.\n",
            static_cast<unsigned long long>(command.sequence.value),
            static_cast<int>(authoritative.result.rejection));
        return false;
    }

    return true;
}

Object* createGuestActor()
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

    dude_stand(actor, obj_dude->rotation, -1);
    return actor;
}

void eraseGuestActor()
{
    if (guestActor == nullptr) {
        return;
    }

    register_clear(guestActor);
    obj_erase_object(guestActor, nullptr);
    guestActor = nullptr;
}

bool beginSession()
{
    guestActor = createGuestActor();
    if (guestActor == nullptr) {
        return false;
    }

    if (session.start(obj_dude, guestActor) != LocalSessionError::None
        || session.transitionTo(SessionPhase::Loading) != LocalSessionError::None
        || session.transitionTo(SessionPhase::Exploration) != LocalSessionError::None) {
        session.stop();
        eraseGuestActor();
        return false;
    }

    CharacterBuild hostBuild;
    if (!captureLegacyCharacterBuild(obj_dude, hostBuild)
        || session.players().setBuild(kHostPlayerId, hostBuild) != PlayerStateError::None
        || session.players().setBuild(kGuestPlayerId, hostBuild) != PlayerStateError::None) {
        session.stop();
        eraseGuestActor();
        return false;
    }

    commandProcessor.reset();
    nextHostCommandSequence = 1;
    nextGuestCommandSequence = 1;

    debug_printf("Multiplayer developer session started with two local actors.\n");
    return true;
}

bool finishMapTransition()
{
    Object* replacement = createGuestActor();
    if (replacement == nullptr) {
        return false;
    }

    if (session.rebindPlayerActor(kGuestPlayerId, replacement) != LocalSessionError::None
        || session.transitionTo(SessionPhase::Exploration) != LocalSessionError::None) {
        obj_erase_object(replacement, nullptr);
        return false;
    }

    guestActor = replacement;
    return true;
}

} // namespace

void developerLocalSessionConfigure(int argc, char** argv)
{
    enabled = false;
    for (int index = 1; index < argc; index++) {
        if (std::strcmp(argv[index], "--multiplayer-dev") == 0) {
            enabled = true;
            break;
        }
    }
}

bool developerLocalSessionIsEnabled()
{
    return enabled;
}

bool developerLocalSessionEnsureStarted()
{
    if (!enabled) {
        return true;
    }

    if (!session.isActive()) {
        return beginSession();
    }

    if (guestActor == nullptr && session.phase() == SessionPhase::Transition) {
        return finishMapTransition();
    }

    return guestActor != nullptr;
}

bool developerLocalSessionSubmitMove(PlayerId playerId, int destinationTile, int elevation, bool running)
{
    return submitCommand(playerId, MoveCommand { destinationTile, elevation, running });
}

bool developerLocalSessionSubmitDoorUse(PlayerId playerId, Object* target)
{
    if (!enabled || !session.isActive() || target == nullptr || !obj_is_a_portal(target)) {
        return false;
    }

    EntityRegistrationResult registered = session.registerWorldObject(target);
    if (!registered) {
        return false;
    }

    return submitCommand(playerId, InteractCommand { registered.entityId });
}

void developerLocalSessionPrepareForWorldReset()
{
    if (!enabled || !session.isActive()) {
        return;
    }

    session.clearWorldEntities();

    if (session.phase() != SessionPhase::Transition
        && session.transitionTo(SessionPhase::Transition) != LocalSessionError::None) {
        debug_printf("Multiplayer developer session stopped during an unexpected world reset.\n");
        eraseGuestActor();
        session.stop();
        commandProcessor.reset();
        return;
    }

    eraseGuestActor();
}

void developerLocalSessionStop()
{
    eraseGuestActor();
    session.stop();
    commandProcessor.reset();
}

} // namespace multiplayer
} // namespace fallout
