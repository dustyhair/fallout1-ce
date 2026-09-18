#include "multiplayer/presentation_bridge.h"

#include "game/anim.h"
#include "game/art.h"
#include "game/inventry.h"
#include "game/object.h"
#include "game/proto.h"
#include "game/proto_types.h"
#include "game/stat.h"
#include "multiplayer/local_player_context.h"

namespace fallout {
namespace multiplayer {

Object* localPlayerActorOrStoryActor()
{
    Object* actor = localPlayerActor();
    return actor != nullptr ? actor : obj_dude;
}

int updatePlayerGenderAppearance(Object* actor)
{
    if (actor == nullptr) {
        return -1;
    }
    if (actor == obj_dude) {
        return proto_dude_update_gender();
    }

    if (inven_worn(actor) == nullptr) {
        int gender = stat_level(actor, STAT_GENDER);
        int artNum = art_vault_person_nums[gender == GENDER_FEMALE ? GENDER_FEMALE : GENDER_MALE];
        int weaponAnimation = 0;
        if (inven_right_hand(actor) != nullptr || inven_left_hand(actor) != nullptr) {
            weaponAnimation = (actor->fid & 0xF000) >> 12;
        }
        int fid = art_id(OBJ_TYPE_CRITTER, artNum, ANIM_STAND, weaponAnimation, actor->rotation + 1);
        return obj_change_fid(actor, fid, nullptr);
    }

    return 0;
}

} // namespace multiplayer
} // namespace fallout
