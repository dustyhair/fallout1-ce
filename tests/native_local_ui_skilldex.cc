#include "game/skill.h"
#include "multiplayer/presentation_bridge.h"
namespace fallout {
static int nativeSkilldexRenderedCount = 0;
static bool nativeSkilldexActorCorrect = true;
int nativeSkilldexRenderedLevel(Object* actor, int skill)
{
    ++nativeSkilldexRenderedCount;
    nativeSkilldexActorCorrect = nativeSkilldexActorCorrect
        && actor == multiplayer::localPlayerActorOrStoryActor();
    return skill_level(actor, skill);
}
}
#define skill_level nativeSkilldexRenderedLevel
#include "game/skilldex.cc"
#undef skill_level
namespace fallout {
bool nativeUiMapBackgroundEnabled();
bool nativeSkilldexBackgroundControl()
{
    map_enable_bk_processes();
    bool before = nativeUiMapBackgroundEnabled();
    gmouse_enable();
    gmouse_disable_scrolling();
    int opened = skilldex_start();
    bool during = nativeUiMapBackgroundEnabled();
    bool mouseBlocked = !gmouse_is_enabled() && !gmouse_scrolling_is_enabled();
    if (opened != -1) skilldex_end();
    bool after = nativeUiMapBackgroundEnabled();
    bool skillActorCorrect = nativeSkilldexRenderedCount == SKILLDEX_SKILL_COUNT && nativeSkilldexActorCorrect
        && multiplayer::localPlayerActorOrStoryActor() != obj_dude;
    fprintf(stderr, "NATIVE_LOCAL_UI_SKILLS local_actor=%d rendered=%d guest_distinct=%d\n", skillActorCorrect, nativeSkilldexRenderedCount, multiplayer::localPlayerActorOrStoryActor() != obj_dude);
    bool mouseRestored = gmouse_is_enabled() && !gmouse_scrolling_is_enabled();
    gmouse_disable(0);
    int nestedOpen = skilldex_start();
    if (nestedOpen != -1) skilldex_end();
    bool nestedPreserved = nestedOpen != -1 && !gmouse_is_enabled() && !gmouse_scrolling_is_enabled();
    gmouse_enable();
    fprintf(stderr, "NATIVE_LOCAL_UI_MOUSE screen=skilldex blocked=%d restored=%d nested_preserved=%d\n", mouseBlocked, mouseRestored, nestedPreserved);
    fprintf(stderr, "NATIVE_LOCAL_UI_BACKGROUND screen=skilldex before=%d opened=%d during=%d after=%d\n", before, opened != -1, during, after);
    return before && opened != -1 && during && after && mouseBlocked && mouseRestored && nestedPreserved && skillActorCorrect;
}
}
