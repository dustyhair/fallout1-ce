#include "game/pipboy.cc"
namespace fallout {
bool nativeUiMapBackgroundEnabled();
bool nativePipboyBackgroundControl()
{
    map_enable_bk_processes();
    bool before=nativeUiMapBackgroundEnabled();
    gmouse_enable();
    gmouse_disable_scrolling();
    int opened=StartPipboy(PIPBOY_OPEN_INTENT_UNSPECIFIED);
    bool during=nativeUiMapBackgroundEnabled();
    bool mouseBlocked = !gmouse_is_enabled() && !gmouse_scrolling_is_enabled();
    if(opened!=-1)EndPipboy();
    bool after=nativeUiMapBackgroundEnabled();
    bool mouseRestored = gmouse_is_enabled() && !gmouse_scrolling_is_enabled();
    const char* originalPath = msg_path;
    msg_path = "missing-native-ui-test/";
    bool cycleBefore = cycle_is_enabled();
    int fontBefore = text_curr();
    int cursorBefore = gmouse_get_cursor();
    bool mouse3dBefore = gmouse_3d_is_on();
    int failedOpen = pipboy(PIPBOY_OPEN_INTENT_UNSPECIFIED);
    msg_path = originalPath;
    bool failureRestored = failedOpen == -1 && nativeUiMapBackgroundEnabled()
        && gmouse_is_enabled() && !gmouse_scrolling_is_enabled()
        && cycle_is_enabled() == cycleBefore && text_curr() == fontBefore
        && gmouse_get_cursor() == cursorBefore && gmouse_3d_is_on() == mouse3dBefore;
    message_exit(&pipboy_message_file);
    gmouse_disable(0);
    int nestedOpen = StartPipboy(PIPBOY_OPEN_INTENT_UNSPECIFIED);
    if (nestedOpen != -1) EndPipboy();
    bool nestedPreserved = nestedOpen != -1 && !gmouse_is_enabled() && !gmouse_scrolling_is_enabled();
    gmouse_enable();
    std::fprintf(stderr, "NATIVE_LOCAL_UI_MOUSE screen=pipboy blocked=%d restored=%d failure_restored=%d nested_preserved=%d\n", mouseBlocked, mouseRestored, failureRestored, nestedPreserved);
    std::fprintf(stderr,"NATIVE_LOCAL_UI_BACKGROUND screen=pipboy before=%d opened=%d during=%d after=%d\n",before,opened!=-1,during,after);
    return before && opened!=-1 && during && after && mouseBlocked && mouseRestored && failureRestored && nestedPreserved;
}
}
