#include "game/pipboy.cc"
namespace fallout {
bool nativeUiMapBackgroundEnabled();
bool nativePipboyBackgroundControl()
{
    map_enable_bk_processes();
    bool before=nativeUiMapBackgroundEnabled();
    int opened=StartPipboy(PIPBOY_OPEN_INTENT_UNSPECIFIED);
    bool during=nativeUiMapBackgroundEnabled();
    if(opened!=-1)EndPipboy();
    bool after=nativeUiMapBackgroundEnabled();
    std::fprintf(stderr,"NATIVE_LOCAL_UI_BACKGROUND screen=pipboy before=%d opened=%d during=%d after=%d\n",before,opened!=-1,during,after);
    return before && opened!=-1 && during && after;
}
}
