#ifndef FALLOUT_MULTIPLAYER_SAVE_SIDECAR_H_
#define FALLOUT_MULTIPLAYER_SAVE_SIDECAR_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "multiplayer/loot_distribution_controller.h"
#include "multiplayer/player_character_state.h"
#include "multiplayer/session_recovery.h"
#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

constexpr std::uint16_t kMultiplayerSaveMinimumVersion = 1;
constexpr std::uint16_t kMultiplayerSaveVersion = 4;
constexpr std::size_t kMultiplayerSaveHeaderSize = 36;
constexpr std::size_t kMultiplayerSaveLegacyPlayerCount = 2;
constexpr std::size_t kMultiplayerSaveMaximumPlayers = kMaximumTransitionPlayers;
constexpr std::size_t kMultiplayerSaveMaximumOwnershipRecords = 4096;
constexpr std::size_t kMultiplayerSaveMaximumActivityEntries = 64;
constexpr std::size_t kMultiplayerSaveMaximumSize = 4 * 1024 * 1024;
constexpr std::uint64_t kMultiplayerSaveDigestOffset = 14695981039346656037ULL;

struct SavedPlayerCharacter {
    PlayerId playerId;
    EntityId actorId;
    std::string name;
    CharacterBuild build;
    std::vector<std::uint8_t> objectData;
    ReconnectToken reconnectToken;
    bool replacementAllowed = false;
};

inline bool operator==(const SavedPlayerCharacter& lhs, const SavedPlayerCharacter& rhs)
{
    return lhs.playerId == rhs.playerId
        && lhs.actorId == rhs.actorId
        && lhs.name == rhs.name
        && lhs.build == rhs.build
        && lhs.objectData == rhs.objectData
        && reconnectTokensEqual(lhs.reconnectToken, rhs.reconnectToken)
        && lhs.replacementAllowed == rhs.replacementAllowed;
}

struct SavedEntityOwnership {
    EntityId entityId;
    PlayerId ownerId;
};

inline bool operator==(const SavedEntityOwnership& lhs,
    const SavedEntityOwnership& rhs)
{
    return lhs.entityId == rhs.entityId && lhs.ownerId == rhs.ownerId;
}

struct MultiplayerSaveSidecar {
    std::uint16_t version = kMultiplayerSaveVersion;
    std::uint64_t generation = 1;
    std::uint64_t saveDatDigest = 0;
    std::uint32_t sessionRules = 0;
    std::vector<SavedPlayerCharacter> players;
    LootDistributionState lootDistribution;
    std::vector<SavedEntityOwnership> ownership;
    std::vector<SharedActivityEntry> sharedActivity;
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
    InvalidPlayerObjectData,
    InvalidReconnectToken,
    InvalidLootState,
    InvalidOwnership,
    InvalidActivity,
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

enum class SavedPlayerSlotResolution {
    ExactReconnect,
    ReplacementAllowed,
    MissingAllowed,
    Rejected,
};

std::uint64_t updateMultiplayerSaveDigest(std::uint64_t digest, const void* data, std::size_t size);
MultiplayerSaveError validateMultiplayerSave(const MultiplayerSaveSidecar& sidecar);
MultiplayerSaveError captureMultiplayerSave(const PlayerCharacterStateStore& players,
    std::uint64_t generation,
    std::uint64_t saveDatDigest,
    MultiplayerSaveSidecar& sidecar);
MultiplayerSaveError encodeMultiplayerSave(const MultiplayerSaveSidecar& sidecar, std::vector<std::uint8_t>& bytes);
MultiplayerSaveDecodeResult decodeMultiplayerSave(const std::vector<std::uint8_t>& bytes);
SavedPlayerSlotResolution resolveSavedPlayerSlot(
    const MultiplayerSaveSidecar& sidecar,
    PlayerId playerId,
    const ReconnectToken& reconnectToken,
    bool playerPresent);
MultiplayerSaveError claimSavedPlayerSlot(MultiplayerSaveSidecar& sidecar,
    PlayerId playerId,
    const ReconnectToken& replacementToken);

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_SAVE_SIDECAR_H_ */
