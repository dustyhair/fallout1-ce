#include "game/editor.cc"
namespace fallout {
bool nativeUiMapBackgroundEnabled();
bool nativeEditorBackgroundControl()
{
    map_enable_bk_processes();
    glblmode = false;
    SavePlayer();
    bool before = nativeUiMapBackgroundEnabled();
    int opened = CharEditStart();
    bool during = nativeUiMapBackgroundEnabled();
    if (opened != -1) CharEditEnd();
    bool after = nativeUiMapBackgroundEnabled();
    fprintf(stderr, "NATIVE_LOCAL_UI_BACKGROUND screen=editor before=%d opened=%d during=%d after=%d\n", before, opened != -1, during, after);
    return before && opened != -1 && during && after;
}
}
