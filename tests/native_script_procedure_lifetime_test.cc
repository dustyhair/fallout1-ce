// Run the real procedure dispatcher with a callback that removes its script.
#define executeProcedure testDestructiveProcedure
#include "game/scripts.cc"
#undef executeProcedure
#include <cstdio>
namespace fallout {
bool GNW95_isActive = false;
char GNW95_title[256];
static int removingSid = -1;
static int executed = 0;
void testDestructiveProcedure(Program*, int)
{
    ++executed;
    scr_remove(removingSid);
}
}
int main()
{
    using namespace fallout;
    queue_init();
    script_engine_running = true;
    Program program {};
    Object removedOwner {};
    Object retainedOwner {};
    Object sourceMarker {};
    auto createRemovingScript = [&]() {
        if (scr_new(&removingSid, SCRIPT_TYPE_CRITTER) != 0) return false;
        Script* script = nullptr;
        if (scr_ptr(removingSid, &script) != 0) return false;
        script->owner = &removedOwner;
        script->program = &program;
        script->scr_flags = SCRIPT_FLAG_0x01;
        script->scr_num_local_vars = 0;
        script->procs[SCRIPT_PROC_TIMED] = 1;
        return true;
    };
    if (!createRemovingScript()) return 2;
    int retainedSid = -1;
    if (scr_new(&retainedSid, SCRIPT_TYPE_CRITTER) != 0) return 2;
    Script* retained = nullptr;
    if (scr_ptr(retainedSid, &retained) != 0) return 2;
    retained->owner = &retainedOwner;
    retained->source = &sourceMarker;
    retained->scr_num_local_vars = 0;
    bool dispatched = exec_script_proc(removingSid, SCRIPT_PROC_TIMED) == 0;
    bool compacted = scr_ptr(retainedSid, &retained) == 0 && retained->source == &sourceMarker;
    scr_remove_all_force();
    if (!createRemovingScript()) return 2;
    bool lastRemoved = exec_script_proc(removingSid, SCRIPT_PROC_TIMED) == 0
        && scriptlists[SCRIPT_TYPE_CRITTER].head == nullptr;
    script_engine_running = false;
    queue_exit();
    std::printf("NATIVE_SCRIPT_PROCEDURE_LIFETIME dispatched=%d retained_source=%d last_removed=%d executed=%d\n",
        dispatched, compacted, lastRemoved, executed);
    return dispatched && compacted && lastRemoved && executed == 2 ? 0 : 1;
}
