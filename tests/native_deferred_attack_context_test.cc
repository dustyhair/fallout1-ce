// Compile the current VM and attack callback. Replace world lookup and the
// final combat request, while retaining the native script registry and stacks.
#define networkWorldActive testWorldActive
#define networkWorldPlayerActor testWorldPlayerActor
#define networkWorldCombatOwner testWorldCombatOwner
#define scripts_request_combat testRequestCombat
#define object_name testObjectName
#define dialog_active testDialogActive
#include "int/intrpret.cc"
#include "int/support/intextra.cc"
#undef dialog_active
#undef object_name
#undef scripts_request_combat
#undef networkWorldCombatOwner
#undef networkWorldPlayerActor
#undef networkWorldActive

#include <cstdio>

namespace fallout {
bool GNW95_isActive = false;
char GNW95_title[256];
static Object hostBody {};
static Object guestBody {};
static Object npcBody {};
static Object* observedTarget = nullptr;
static bool worldActive = true;
char* testObjectName(Object*) { static char name[] = "fixture"; return name; }
bool testDialogActive() { return false; }
int testRequestCombat(STRUCT_664980* attack)
{
    observedTarget = attack->defender;
    return 0;
}
namespace multiplayer {
bool testWorldActive() { return worldActive; }
Object* testWorldPlayerActor(PlayerId id)
{
    return id == kHostPlayerId ? &hostBody : id == kGuestPlayerId ? &guestBody : nullptr;
}
std::optional<PlayerId> testWorldCombatOwner(const Object* actor)
{
    if (actor == &hostBody) return kHostPlayerId;
    if (actor == &guestBody) return kGuestPlayerId;
    return std::nullopt;
}
} // namespace multiplayer

static ProgramValue copiedThroughVm(ProgramValue value, int mode)
{
    ProgramValue zero;
    zero.opcode = VALUE_TYPE_INT;
    zero.integerValue = 0;
    ProgramStack stack { zero };
    ProgramStack returns;
    Program scratch {};
    scratch.stackValues = &stack;
    scratch.returnStackValues = &returns;
    programStackPushValue(&scratch, value);
    if (mode == 0 || mode == 1) {
        programStackPushInteger(&scratch, 0);
        if (mode == 0) op_store(&scratch); else op_store_global(&scratch);
        programStackPushInteger(&scratch, 0);
        if (mode == 0) op_fetch(&scratch); else op_fetch_global(&scratch);
    } else if (mode == 2) {
        programStackPushInteger(&scratch, 27);
        op_swap(&scratch);
    } else {
        op_d_to_a(&scratch);
        op_a_to_d(&scratch);
    }
    return programStackPopValue(&scratch);
}
} // namespace fallout

int main()
{
    using namespace fallout;
    using namespace fallout::multiplayer;
    hostBody.pid = guestBody.pid = npcBody.pid = 0x01000001;
    obj_dude = &hostBody;
    LocalSession session;
    if (session.start(&hostBody, &guestBody) != LocalSessionError::None
        || bindLocalPlayer(session, kHostPlayerId) != LocalPlayerError::None) return 2;
    int sid = -1;
    if (scr_new(&sid, SCRIPT_TYPE_CRITTER) != 0) return 2;
    Script* script = nullptr;
    if (scr_ptr(sid, &script) != 0) return 2;
    ProgramStack stack;
    ProgramStack returns;
    Program program {};
    program.stackValues = &stack;
    program.returnStackValues = &returns;
    script->program = &program;
    script->owner = &npcBody;
    script->action = SCRIPT_PROC_CRITTER;
    bool passed = true;
    auto remember = [&](Object* actor) {
        intExtraClearDialogueActor(&program);
        intExtraRememberDialogueActor(&program, actor);
    };
    auto implicit = [&]() {
        op_dude_obj(&program);
        return programStackPopValue(&program);
    };
    auto explicitPointer = [&](Object* actor) {
        programStackPushPointer(&program, actor);
        return programStackPopValue(&program);
    };
    auto check = [&](const char* name, ProgramValue argument, Object* expected) {
        observedTarget = nullptr;
        programStackPushValue(&program, argument);
        for (int i = 0; i < 7; ++i) programStackPushInteger(&program, 0);
        op_attack(&program);
        bool good = observedTarget == expected && stack.empty() && returns.empty();
        std::printf("NATIVE_DEFERRED_ATTACK_%s case=%s target=%s stack_restored=%d\n",
            good ? "PASS" : "FAIL", name,
            observedTarget == &hostBody ? "host" : observedTarget == &guestBody ? "guest" : "other",
            stack.empty() && returns.empty());
        passed = passed && good;
    };
    remember(&guestBody); check("explicit_host_guest_origin", explicitPointer(&hostBody), &hostBody);
    remember(&hostBody); check("explicit_guest_host_origin", explicitPointer(&guestBody), &guestBody);
    remember(&guestBody); implicit(); check("popped_implicit_reused_slot", explicitPointer(&hostBody), &hostBody);
    remember(&guestBody); check("implicit_guest_origin", implicit(), &guestBody);
    remember(&guestBody); intExtraRemoveProgramReferences(&program);
    check("removed_program_origin", implicit(), &hostBody);
    for (int mode = 0; mode < 4; ++mode) {
        const char* names[] = { "local_copy", "global_copy", "swap_copy", "return_stack_copy" };
        remember(&guestBody); check(names[mode], copiedThroughVm(implicit(), mode), &guestBody);
    }
    remember(&hostBody);
    ProgramValue oldHostValue = copiedThroughVm(implicit(), 0);
    remember(&guestBody);
    check("cached_old_dialogue_value", oldHostValue, &hostBody);
    remember(&hostBody);
    ProgramValue oldSessionValue = copiedThroughVm(implicit(), 1);
    intExtraResetDialogueActors();
    remember(&guestBody);
    check("cached_old_session_value", oldSessionValue, &hostBody);
    remember(&guestBody);
    intExtraResetDialogueActors();
    check("session_reset_live_program", implicit(), &hostBody);
    remember(&guestBody);
    check("new_session_origin", implicit(), &guestBody);
    intExtraClearDialogueActor(&program);
    script->program = nullptr;
    scr_remove_all_force();
    session.stop();
    return passed ? 0 : 1;
}
