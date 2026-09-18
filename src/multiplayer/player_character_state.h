#ifndef FALLOUT_MULTIPLAYER_PLAYER_CHARACTER_STATE_H_
#define FALLOUT_MULTIPLAYER_PLAYER_CHARACTER_STATE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "game/perk_defs.h"
#include "game/skill_defs.h"
#include "game/stat_defs.h"
#include "game/trait.h"
#include "multiplayer/entity_registry.h"
#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

struct CharacterBuild {
    std::array<std::int32_t, SAVEABLE_STAT_COUNT> baseStats {};
    std::array<std::int32_t, SAVEABLE_STAT_COUNT> bonusStats {};
    std::array<std::int32_t, SKILL_COUNT> skillPoints {};
    std::array<std::int32_t, PERK_COUNT> perkRanks {};
    std::array<std::int32_t, NUM_TAGGED_SKILLS> taggedSkills { -1, -1, -1, -1 };
    std::array<std::int32_t, PC_TRAIT_MAX> traits { -1, -1 };
    std::uint32_t prototypeFlags = 0;
    std::int32_t unspentSkillPoints = 0;
    std::int32_t level = 1;
    std::int32_t experience = 0;
};

inline bool operator==(const CharacterBuild& lhs, const CharacterBuild& rhs)
{
    return lhs.baseStats == rhs.baseStats
        && lhs.bonusStats == rhs.bonusStats
        && lhs.skillPoints == rhs.skillPoints
        && lhs.perkRanks == rhs.perkRanks
        && lhs.taggedSkills == rhs.taggedSkills
        && lhs.traits == rhs.traits
        && lhs.prototypeFlags == rhs.prototypeFlags
        && lhs.unspentSkillPoints == rhs.unspentSkillPoints
        && lhs.level == rhs.level
        && lhs.experience == rhs.experience;
}

inline bool operator!=(const CharacterBuild& lhs, const CharacterBuild& rhs)
{
    return !(lhs == rhs);
}

enum class PlayerOwnership {
    LocalControl,
    RemoteControl,
};

enum class ConnectionState {
    Local,
    Connected,
    Disconnected,
};

struct PlayerCharacterState {
    PlayerId id;
    EntityId actorId;
    CharacterBuild build;
    PlayerOwnership ownership = PlayerOwnership::RemoteControl;
    ConnectionState connection = ConnectionState::Disconnected;
};

enum class PlayerStateError {
    None,
    InvalidPlayerId,
    InvalidEntityId,
    ActorMissing,
    ActorNotOwned,
    PlayerAlreadyRegistered,
    ActorAlreadyRegistered,
    PlayerNotFound,
};

class PlayerCharacterStateStore {
public:
    PlayerStateError registerPlayer(PlayerCharacterState state, const EntityRegistry& entities);
    PlayerStateError setBuild(PlayerId playerId, const CharacterBuild& build);
    PlayerStateError setConnection(PlayerId playerId, ConnectionState connection);
    PlayerStateError unregisterPlayer(PlayerId playerId);

    PlayerCharacterState* find(PlayerId playerId);
    const PlayerCharacterState* find(PlayerId playerId) const;
    PlayerCharacterState* findByActor(EntityId actorId);
    const PlayerCharacterState* findByActor(EntityId actorId) const;

    bool bindingsMatch(const EntityRegistry& entities) const;
    std::size_t size() const;
    void clear();

private:
    std::unordered_map<PlayerId, PlayerCharacterState, PlayerIdHash> _players;
    std::unordered_map<EntityId, PlayerId, EntityIdHash> _actors;
};

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_PLAYER_CHARACTER_STATE_H_ */
