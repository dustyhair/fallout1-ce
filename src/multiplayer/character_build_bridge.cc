#include "multiplayer/character_build_bridge.h"

#include "game/perk.h"
#include "game/proto.h"
#include "game/skill.h"
#include "game/stat.h"
#include "game/trait.h"

namespace fallout {
namespace multiplayer {

bool captureLegacyCharacterBuild(Object* actor, CharacterBuild& build)
{
    if (actor == nullptr) {
        return false;
    }

    Proto* proto = nullptr;
    if (proto_ptr(actor->pid, &proto) == -1 || proto == nullptr || PID_TYPE(actor->pid) != OBJ_TYPE_CRITTER) {
        return false;
    }

    for (int stat = 0; stat < SAVEABLE_STAT_COUNT; stat++) {
        build.baseStats[stat] = proto->critter.data.baseStats[stat];
        build.bonusStats[stat] = proto->critter.data.bonusStats[stat];
    }
    for (int skill = 0; skill < SKILL_COUNT; skill++) {
        build.skillPoints[skill] = proto->critter.data.skills[skill];
    }
    for (int perk = 0; perk < PERK_COUNT; perk++) {
        build.perkRanks[perk] = perk_level(perk);
    }

    int taggedSkills[NUM_TAGGED_SKILLS];
    skill_get_tags(taggedSkills, NUM_TAGGED_SKILLS);
    for (int index = 0; index < NUM_TAGGED_SKILLS; index++) {
        build.taggedSkills[index] = taggedSkills[index];
    }

    int firstTrait;
    int secondTrait;
    trait_get(&firstTrait, &secondTrait);
    build.traits[0] = firstTrait;
    build.traits[1] = secondTrait;

    build.prototypeFlags = static_cast<std::uint32_t>(proto->critter.data.flags);
    build.unspentSkillPoints = stat_pc_get(PC_STAT_UNSPENT_SKILL_POINTS);
    build.level = stat_pc_get(PC_STAT_LEVEL);
    build.experience = stat_pc_get(PC_STAT_EXPERIENCE);
    return true;
}

} // namespace multiplayer
} // namespace fallout
