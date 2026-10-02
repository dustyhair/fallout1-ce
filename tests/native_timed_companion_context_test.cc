// Compile the current native dispatcher and observe its execution context.
// The test supplies membership/roster predicates and replaces only the VM call.
#define isPartyMember testIsPartyMember
#define networkWorldActive testWorldActive
#define networkWorldPlayerActor testWorldPlayerActor
#define networkWorldCombatOwner testWorldCombatOwner
#define executeProcedure testExecuteProcedure
#include "game/scripts.cc"
#undef executeProcedure
#undef networkWorldCombatOwner
#undef networkWorldPlayerActor
#undef networkWorldActive
#undef isPartyMember

#include <cstdio>

namespace fallout {
bool GNW95_isActive = false;
char GNW95_title[256];

static Object hostBody {};
static Object guestBody {};
static Object npcBody {};
static Object nonpartyBody {};
static Object* observedActing = nullptr;
static Object* observedLocal = nullptr;
static int observedParam = 0;
static int observedProcedure = 0;
static int observedSid = -1;
static bool worldActive = true;

bool testIsPartyMember(Object* actor)
{
    // Include both roster actors deliberately. Roster exclusion must prevail
    // even when native membership recognizes a human leader slot.
    return actor == &npcBody || actor == &hostBody || actor == &guestBody;
}

void testExecuteProcedure(Program* program, int procedure)
{
    Script* script = nullptr;
    if (scr_ptr(observedSid, &script) != 0 || script->program != program) return;
    observedActing = multiplayer::actingPlayerActor();
    observedLocal = multiplayer::localPlayerActor();
    observedParam = script->fixedParam;
    observedProcedure = procedure;
}

namespace multiplayer {
bool testWorldActive()
{
    return worldActive;
}

Object* testWorldPlayerActor(PlayerId player)
{
    return player == kHostPlayerId ? &hostBody : &guestBody;
}

std::optional<PlayerId> testWorldCombatOwner(const Object* actor)
{
    if (actor == &hostBody) return kHostPlayerId;
    if (actor == &guestBody) return kGuestPlayerId;
    return std::nullopt;
}
} // namespace multiplayer
} // namespace fallout

int main()
{
    using namespace fallout;
    using namespace fallout::multiplayer;
    LocalSession session;
    if (session.start(&hostBody, &guestBody) != LocalSessionError::None
        || bindLocalPlayer(session, kHostPlayerId) != LocalPlayerError::None) return 2;
    if (scr_new(&observedSid, SCRIPT_TYPE_CRITTER) != 0) return 2;
    Script* script = nullptr;
    if (scr_ptr(observedSid, &script) != 0) return 2;
    Program program {};
    script->program = &program;
    script->scr_flags = SCRIPT_FLAG_0x01;
    script->procs[SCRIPT_PROC_TIMED] = SCRIPT_PROC_TIMED;
    script_engine_running = true;
    auto* guest = session.players().find(kGuestPlayerId);
    bool passed = true;
    auto check = [&](const char* name, Object* owner, Object* expected) {
        script->owner = owner;
        bool selected = false;
        bool nestedRestored = false;
        {
            ScopedActingPlayerContext action(*guest, &guestBody);
            ScopedLocalPlayerBinding binding(&guestBody);
            ScriptEvent event { observedSid, 271 };
            observedActing = nullptr;
            observedLocal = nullptr;
            observedParam = 0;
            observedProcedure = 0;
            selected = script_q_process(owner, &event) == 0
                && observedActing == expected && observedLocal == expected
                && observedParam == 271 && observedProcedure == SCRIPT_PROC_TIMED
                && script->action == SCRIPT_PROC_TIMED;
            nestedRestored = actingPlayerActor() == &guestBody
                && localPlayerActor() == &guestBody;
        }
        bool outerRestored = actingPlayerActor() == nullptr
            && localPlayerActor() == &hostBody;
        bool thisPassed = selected && nestedRestored && outerRestored;
        std::printf("NATIVE_TIMED_OWNER_CONTEXT_%s case=%s selected=%s fixed_param=%d native_dispatch=1 nested_scope_restored=%d outer_scope_restored=%d\n",
            thisPassed ? "PASS" : "FAIL", name,
            observedActing == &hostBody ? "host" : observedActing == &guestBody ? "guest" : "other",
            observedParam, nestedRestored, outerRestored);
        passed = passed && thisPassed;
    };
    check("npc_party", &npcBody, &hostBody);
    check("host_human_party", &hostBody, &guestBody);
    check("guest_human_party", &guestBody, &guestBody);
    check("nonparty", &nonpartyBody, &guestBody);
    check("null_owner", nullptr, &guestBody);
    worldActive = false;
    check("inactive_npc_party", &npcBody, &guestBody);
    // The program is stack-owned test state, not an allocated native program.
    script->program = nullptr;
    script->scr_flags = 0;
    scr_remove_all_force();
    script_engine_running = false;
    session.stop();
    return passed ? 0 : 1;
}
