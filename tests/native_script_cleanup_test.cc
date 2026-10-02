#include "game/scripts.cc"

#include <cstdio>

namespace fallout {
bool GNW95_isActive = false;
char GNW95_title[256];
}

int main()
{
    using namespace fallout;
    queue_init();
    Object owner {};
    constexpr int count = 40;
    int protectedId = -1;
    int lastProtectedId = -1;
    for (int index = 0; index < count; index++) {
        int sid = -1;
        if (scr_new(&sid, SCRIPT_TYPE_CRITTER) != 0) return 2;
        Script* script = nullptr;
        if (scr_ptr(sid, &script) != 0) return 2;
        script->scr_flags = SCRIPT_FLAG_0x04;
        script->scr_num_local_vars = 0;
        script->program = nullptr;
        script->owner = &owner;
        if (index == 0) {
            protectedId = sid;
            script->scr_flags = SCRIPT_FLAG_0x08 | SCRIPT_FLAG_0x10;
        } else if (index == 1) {
            // Native party recovery can leave an unprotected slot with the
            // protected player's same SID and owner. Bulk cleanup must remove
            // this exact slot, rather than repeatedly resolving the first SID.
            script->scr_id = protectedId;
        } else if (index == count - 1) {
            lastProtectedId = sid;
            script->scr_flags = SCRIPT_FLAG_0x08 | SCRIPT_FLAG_0x10;
        }
    }
    bool removed = scr_remove_all() == 0;
    int remaining = 0;
    bool protectedOnly = true;
    auto& list = scriptlists[SCRIPT_TYPE_CRITTER];
    for (auto* extent = list.head; extent != nullptr; extent = extent->next) {
        for (int index = 0; index < extent->length; index++) {
            const auto& script = extent->scripts[index];
            ++remaining;
            protectedOnly = protectedOnly && (script.scr_flags & SCRIPT_FLAG_0x10) != 0
                && (script.scr_id == protectedId || script.scr_id == lastProtectedId);
        }
    }
    bool repeated = scr_remove_all() == 0;
    bool clearLast = scr_remove_all_force() == 0 && list.head == nullptr
        && list.tail == nullptr && list.length == 0;
    queue_exit();
    std::printf("NATIVE_SCRIPT_CLEANUP removed=%d remaining=%d protected_only=%d repeated=%d force_clear=%d\n",
        removed, remaining, protectedOnly, repeated, clearLast);
    return removed && remaining == 2 && protectedOnly && repeated && clearLast ? 0 : 1;
}
