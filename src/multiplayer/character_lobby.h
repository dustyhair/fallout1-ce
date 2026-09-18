#ifndef FALLOUT_MULTIPLAYER_CHARACTER_LOBBY_H_
#define FALLOUT_MULTIPLAYER_CHARACTER_LOBBY_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "game/skill_defs.h"
#include "game/stat_defs.h"
#include "game/trait.h"
#include "multiplayer/player_character_state.h"
#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

constexpr std::uint16_t kCharacterSheetVersion = 1;
constexpr std::size_t kCharacterNameMaxLength = 31;
constexpr std::size_t kCharacterSheetMinimumPacketSize = 63;

struct CharacterCreationSheet {
    std::uint16_t version = kCharacterSheetVersion;
    PlayerId playerId;
    std::string name;
    std::array<std::int32_t, PRIMARY_STAT_COUNT> primaryStats { 5, 5, 5, 5, 5, 5, 5 };
    std::int32_t age = 25;
    std::int32_t gender = 0;
    std::array<std::int32_t, DEFAULT_TAGGED_SKILLS> taggedSkills { -1, -1, -1 };
    std::array<std::int32_t, PC_TRAIT_MAX> traits { -1, -1 };
};

inline bool operator==(const CharacterCreationSheet& lhs, const CharacterCreationSheet& rhs)
{
    return lhs.version == rhs.version
        && lhs.playerId == rhs.playerId
        && lhs.name == rhs.name
        && lhs.primaryStats == rhs.primaryStats
        && lhs.age == rhs.age
        && lhs.gender == rhs.gender
        && lhs.taggedSkills == rhs.taggedSkills
        && lhs.traits == rhs.traits;
}

enum class CharacterLobbyError {
    None,
    SessionInactive,
    WrongPhase,
    UnsupportedVersion,
    InvalidPlayerId,
    PlayerMissing,
    EmptyName,
    NameTooLong,
    InvalidName,
    PrimaryStatOutOfRange,
    InvalidPrimaryStatTotal,
    AgeOutOfRange,
    InvalidGender,
    InvalidTaggedSkill,
    DuplicateTaggedSkill,
    InvalidTrait,
    DuplicateTrait,
    NonCanonicalTraits,
    PacketTooShort,
    TruncatedPacket,
    TrailingData,
};

struct CharacterSheetDecodeResult {
    CharacterLobbyError error = CharacterLobbyError::None;
    CharacterCreationSheet sheet;

    explicit operator bool() const
    {
        return error == CharacterLobbyError::None;
    }
};

CharacterLobbyError validateCharacterSheet(const CharacterCreationSheet& sheet);
CharacterBuild characterBuildFromSheet(const CharacterCreationSheet& sheet);
CharacterCreationSheet characterSheetFromBuild(PlayerId playerId, const std::string& name, const CharacterBuild& build);
CharacterLobbyError encodeCharacterSheet(const CharacterCreationSheet& sheet, std::vector<std::uint8_t>& packet);
CharacterSheetDecodeResult decodeCharacterSheet(const std::vector<std::uint8_t>& packet);

class CharacterLobby {
public:
    CharacterLobbyError submit(const CharacterCreationSheet& sheet, PlayerCharacterStateStore& players);
    bool isReady(PlayerId playerId) const;
    bool allPlayersReady() const;
    const CharacterCreationSheet* sheet(PlayerId playerId) const;
    void reset();

private:
    std::optional<CharacterCreationSheet> _hostSheet;
    std::optional<CharacterCreationSheet> _guestSheet;
};

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_CHARACTER_LOBBY_H_ */
