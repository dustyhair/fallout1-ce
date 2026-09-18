#include "multiplayer/developer_local_session.h"

#include <cstring>

#include "game/anim.h"
#include "game/object.h"
#include "game/protinst.h"
#include "multiplayer/local_session.h"
#include "plib/gnw/debug.h"

namespace fallout {
namespace multiplayer {
namespace {

bool enabled = false;
Object* guestActor = nullptr;
LocalSession session;

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

void developerLocalSessionPrepareForWorldReset()
{
    if (!enabled || !session.isActive()) {
        return;
    }

    if (session.phase() != SessionPhase::Transition
        && session.transitionTo(SessionPhase::Transition) != LocalSessionError::None) {
        debug_printf("Multiplayer developer session stopped during an unexpected world reset.\n");
        eraseGuestActor();
        session.stop();
        return;
    }

    eraseGuestActor();
}

void developerLocalSessionStop()
{
    eraseGuestActor();
    session.stop();
}

} // namespace multiplayer
} // namespace fallout
