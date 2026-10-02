#include "game/options.cc"
namespace fallout {
bool nativeUiMapBackgroundEnabled();
bool nativeOptionsBackgroundControl()
{
    map_enable_bk_processes();
    bool before = nativeUiMapBackgroundEnabled();
    gmouse_enable();
    gmouse_disable_scrolling();
    int opened = OptnStart();
    bool during = nativeUiMapBackgroundEnabled();
    bool mouseBlocked = !gmouse_is_enabled() && !gmouse_scrolling_is_enabled();
    if (opened != -1) OptnEnd();
    bool after = nativeUiMapBackgroundEnabled();
    bool mouseRestored = gmouse_is_enabled() && !gmouse_scrolling_is_enabled();
    gmouse_disable(0);
    int nestedOpen = OptnStart();
    if (nestedOpen != -1) OptnEnd();
    bool nestedPreserved = nestedOpen != -1 && !gmouse_is_enabled() && !gmouse_scrolling_is_enabled();
    gmouse_enable();
    fprintf(stderr, "NATIVE_LOCAL_UI_MOUSE screen=options blocked=%d restored=%d nested_preserved=%d\n", mouseBlocked, mouseRestored, nestedPreserved);
    fprintf(stderr, "NATIVE_LOCAL_UI_BACKGROUND screen=options before=%d opened=%d during=%d after=%d\n", before, opened != -1, during, after);
    return before && opened != -1 && during && after && mouseBlocked && mouseRestored && nestedPreserved;
}
}
