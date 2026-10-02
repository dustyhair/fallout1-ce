#include "game/intface.cc"
#include <vector>
#include <utility>
namespace fallout {
bool nativePausedHudControl()
{
    multiplayer::ScopedLocalPlayerContext context;
    Object* player = intface_player();
    if (player == nullptr) return false;
    Object* weapon = nullptr;
    std::vector<std::pair<Object*, int>> flags;
    for (int index = 0; index < player->data.inventory.length; ++index) {
        Object* item = player->data.inventory.items[index].item;
        flags.emplace_back(item, item->flags);
        item->flags &= ~OBJECT_IN_LEFT_HAND;
        if (weapon == nullptr && item_get_type(item) == ITEM_TYPE_WEAPON) weapon = item;
    }
    if (weapon == nullptr) return false;
    int savedHand = itemCurrentItem;
    itemCurrentItem = HAND_LEFT;
    intface_update_items(false);
    weapon->flags |= OBJECT_IN_LEFT_HAND;
    map_enable_bk_processes();
    map_disable_bk_processes();
    bool changingBefore = intface_fid_is_changing;
    int busyBefore = anim_busy(player);
    unsigned int started = get_time();
    int result = intface_update_items(true);
    unsigned int elapsed = get_time() - started;
    bool paused = !map_bk_processes_enabled();
    bool idle = intface_fid_is_changing == changingBefore && anim_busy(player) == busyBefore;
    bool refreshed = itemButtonItems[HAND_LEFT].item == weapon;
    map_enable_bk_processes();
    for (const auto& entry : flags) entry.first->flags = entry.second;
    itemCurrentItem = savedHand;
    intface_update_items(false);
    fprintf(stderr, "NATIVE_PAUSED_HUD_CONTROL changed_item=1 result=%d paused=%d animation_unchanged=%d refreshed=%d elapsed_ms=%u\n",
        result, paused, idle, refreshed, elapsed);
    return result == 0 && paused && idle && refreshed && elapsed < 1000;
}
}
