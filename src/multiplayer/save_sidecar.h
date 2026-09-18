#ifndef FALLOUT_MULTIPLAYER_SAVE_SIDECAR_H_
#define FALLOUT_MULTIPLAYER_SAVE_SIDECAR_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "multiplayer/player_character_state.h"
#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

constexpr std::uint16_t kMultiplayerSaveVersion = 1;
constexpr std::size_t kMultiplayerSaveHeaderSize = 36;
constexpr std::size_t kMultiplayerSavePlayerCount = 2;
constexpr std::size_t kMultiplayerSaveMaximumSize = 64 * 1024;
constexpr std::uint64_t kMultiplayerSaveDigestOffset = 14695981039346656037ULL;

struct SavedPlayerCharacter {
    PlayerId playerId;
    std::string name;
    CharacterBuild build;
};

inline bool operator==(const SavedPlayerCharacter& lhs, const SavedPlayerCharacter& rhs)
{
    return lhs.playerId == rhs.playerId
        && lhs.name == rhs.name
        && lhs.build == rhs.build;
}

struct MultiplayerSaveSidecar {
    std::uint16_t version = kMultiplayerSaveVersion;
    std::uint64_t generation = 1;
    std::uint64_t saveDatDigest = 0;
    std::array<SavedPlayerCharacter, kMultiplayerSavePlayerCount> players;
};

enum class MultiplayerSaveError {
    None,
    PacketTooShort,
    InvalidMagic,
    UnsupportedVersion,
    InvalidHeader,
    InvalidGeneration,
    InvalidPlayerCount,
    InvalidPlayerId,
    DuplicatePlayer,
    NonCanonicalPlayerOrder,
    PlayerMissing,
    EmptyName,
    NameTooLong,
    InvalidName,
    InvalidBuild,
    PayloadTooLarge,
    TruncatedPayload,
    TrailingData,
    ChecksumMismatch,
};

struct MultiplayerSaveDecodeResult {
    MultiplayerSaveError error = MultiplayerSaveError::None;
    MultiplayerSaveSidecar sidecar;

    explicit operator bool() const
    {
        return error == MultiplayerSaveError::None;
    }
};

std::uint64_t updateMultiplayerSaveDigest(std::uint64_t digest, const void* data, std::size_t size);
MultiplayerSaveError validateMultiplayerSave(const MultiplayerSaveSidecar& sidecar);
MultiplayerSaveError captureMultiplayerSave(const PlayerCharacterStateStore& players,
    std::uint64_t generation,
    std::uint64_t saveDatDigest,
    MultiplayerSaveSidecar& sidecar);
MultiplayerSaveError encodeMultiplayerSave(const MultiplayerSaveSidecar& sidecar, std::vector<std::uint8_t>& bytes);
MultiplayerSaveDecodeResult decodeMultiplayerSave(const std::vector<std::uint8_t>& bytes);

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_SAVE_SIDECAR_H_ */
