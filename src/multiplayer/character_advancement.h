#ifndef FALLOUT_MULTIPLAYER_CHARACTER_ADVANCEMENT_H_
#define FALLOUT_MULTIPLAYER_CHARACTER_ADVANCEMENT_H_

#include <algorithm>
#include "multiplayer/player_character_state.h"

namespace fallout {
namespace multiplayer {

// Intelligence includes native trait adjustments, but not temporary bonuses.
inline void grantCharacterLevels(CharacterBuild& build, int intelligence)
{
    bool gifted = std::find(build.traits.begin(), build.traits.end(), TRAIT_GIFTED) != build.traits.end();
    bool skilled = std::find(build.traits.begin(), build.traits.end(), TRAIT_SKILLED) != build.traits.end();
    int selected = std::count_if(build.perkRanks.begin(), build.perkRanks.end(), [](int rank) { return rank > 0; });
    for (int level = build.processedLevel + 1; level <= build.level && level <= PC_LEVEL_MAX; ++level) {
        int points = 5 + intelligence * 2 + build.perkRanks[PERK_EDUCATED] * 2 - (gifted ? 5 : 0);
        build.unspentSkillPoints = std::clamp(build.unspentSkillPoints + points, 0, 99);
        if (selected < 7 && level % (skilled ? 4 : 3) == 0) {
            ++build.pendingPerks;
        }
        build.processedLevel = level;
    }
}

// Only advancement inputs participate. Sneak timers, healing limits, and other
// transient state can change while the player reads the editor.
inline std::uint64_t characterAdvancementFingerprint(const CharacterBuild& build)
{
    std::uint64_t hash = 14695981039346656037ULL;
    auto add = [&](int value) {
        for (int shift = 0; shift < 32; shift += 8) {
            hash ^= (static_cast<std::uint32_t>(value) >> shift) & 0xff;
            hash *= 1099511628211ULL;
        }
    };
    for (int value : build.baseStats) add(value);
    for (int value : build.bonusStats) add(value);
    for (int value : build.skillPoints) add(value);
    for (int value : build.perkRanks) add(value);
    for (int value : build.taggedSkills) add(value);
    for (int value : build.traits) add(value);
    add(build.unspentSkillPoints);
    add(build.level);
    add(build.experience);
    add(build.processedLevel);
    add(build.pendingPerks);
    return hash;
}

} // namespace multiplayer
} // namespace fallout
#endif
