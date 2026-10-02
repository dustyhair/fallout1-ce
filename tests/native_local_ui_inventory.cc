#include "game/inventry.cc"
namespace fallout {
bool nativeUiMapBackgroundEnabled();
bool nativeInventoryBackgroundControl()
{
    map_enable_bk_processes();
    inven_reset_dude();
    if(inven_init()!=0)return false;
    bool previousNetwork=networkInventory;
    networkInventory=true;
    bool before=nativeUiMapBackgroundEnabled();
    bool wasEnabled=setup_inventory(INVENTORY_WINDOW_TYPE_NORMAL);
    bool during=nativeUiMapBackgroundEnabled();
    exit_inventory(wasEnabled);
    inven_exit();
    networkInventory=previousNetwork;
    bool after=nativeUiMapBackgroundEnabled();
    std::fprintf(stderr,"NATIVE_LOCAL_UI_BACKGROUND screen=inventory before=%d during=%d after=%d\n",before,during,after);
    return before && during && after;
}
}
