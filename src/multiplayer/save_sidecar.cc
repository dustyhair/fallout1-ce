#include "multiplayer/save_sidecar.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "multiplayer/character_lobby.h"

namespace fallout {
namespace multiplayer {
namespace {

constexpr std::uint8_t kMagic[] = { 'F', 'C', 'M', 'D' };
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::int32_t kMaximumStoredValue = 1000000;

void appendUInt16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void appendUInt32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void appendUInt64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
    appendUInt32(bytes, static_cast<std::uint32_t>(value >> 32));
    appendUInt32(bytes, static_cast<std::uint32_t>(value));
}

void appendInt32(std::vector<std::uint8_t>& bytes, std::int32_t value)
{
    appendUInt32(bytes, static_cast<std::uint32_t>(value));
}

std::uint16_t readUInt16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8)
        | static_cast<std::uint16_t>(bytes[offset + 1]));
}

std::uint32_t readUInt32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24)
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8)
        | static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint64_t readUInt64(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (static_cast<std::uint64_t>(readUInt32(bytes, offset)) << 32)
        | readUInt32(bytes, offset + 4);
}

class PayloadReader {
public:
    PayloadReader(const std::vector<std::uint8_t>& bytes, std::size_t offset)
        : _bytes(bytes)
        , _offset(offset)
    {
    }

    bool readUInt8(std::uint8_t& value)
    {
        if (!has(1)) {
            return false;
        }
        value = _bytes[_offset++];
        return true;
    }

    bool readUInt16(std::uint16_t& value)
    {
        if (!has(2)) {
            return false;
        }
        value = fallout::multiplayer::readUInt16(_bytes, _offset);
        _offset += 2;
        return true;
    }

    bool readUInt32(std::uint32_t& value)
    {
        if (!has(4)) {
            return false;
        }
        value = fallout::multiplayer::readUInt32(_bytes, _offset);
        _offset += 4;
        return true;
    }

    bool readInt32(std::int32_t& value)
    {
        std::uint32_t raw;
        if (!readUInt32(raw)) {
            return false;
        }
        value = static_cast<std::int32_t>(raw);
        return true;
    }

    bool readString(std::size_t length, std::string& value)
    {
        if (!has(length)) {
            return false;
        }
        value.assign(_bytes.begin() + _offset, _bytes.begin() + _offset + length);
        _offset += length;
        return true;
    }

    bool readBytes(std::size_t length, std::vector<std::uint8_t>& value)
    {
        if (!has(length)) {
            return false;
        }
        value.assign(_bytes.begin() + _offset, _bytes.begin() + _offset + length);
        _offset += length;
        return true;
    }

    bool finished() const
    {
        return _offset == _bytes.size();
    }

private:
    bool has(std::size_t size) const
    {
        return size <= _bytes.size() - _offset;
    }

    const std::vector<std::uint8_t>& _bytes;
    std::size_t _offset;
};

template <std::size_t Size>
void appendInt32Array(std::vector<std::uint8_t>& bytes, const std::array<std::int32_t, Size>& values)
{
    for (std::int32_t value : values) {
        appendInt32(bytes, value);
    }
}

template <std::size_t Size>
bool readInt32Array(PayloadReader& reader, std::array<std::int32_t, Size>& values)
{
    for (std::int32_t& value : values) {
        if (!reader.readInt32(value)) {
            return false;
        }
    }
    return true;
}

bool isValidName(const std::string& name)
{
    if (name.empty() || name.size() > kCharacterNameMaxLength) {
        return false;
    }

    bool visible = false;
    for (unsigned char ch : name) {
        if (ch < 32 || ch > 126) {
            return false;
        }
        visible = visible || !std::isspace(ch);
    }
    return visible;
}

bool isBounded(std::int32_t value)
{
    return value >= -kMaximumStoredValue && value <= kMaximumStoredValue;
}

bool isValidBuild(const CharacterBuild& build)
{
    if (!std::all_of(build.baseStats.begin(), build.baseStats.end(), isBounded)
        || !std::all_of(build.bonusStats.begin(), build.bonusStats.end(), isBounded)) {
        return false;
    }
    for (std::int32_t value : build.skillPoints) {
        if (value < 0 || value > kMaximumStoredValue) {
            return false;
        }
    }
    for (std::int32_t rank : build.perkRanks) {
        if (rank < 0 || rank > kMaximumStoredValue) {
            return false;
        }
    }
    for (std::size_t index = 0; index < build.taggedSkills.size(); index++) {
        std::int32_t skill = build.taggedSkills[index];
        if (skill < -1 || skill >= SKILL_COUNT) {
            return false;
        }
        for (std::size_t previous = 0; previous < index; previous++) {
            if (skill != -1 && build.taggedSkills[previous] == skill) {
                return false;
            }
        }
    }
    for (std::size_t index = 0; index < build.traits.size(); index++) {
        std::int32_t trait = build.traits[index];
        if (trait < -1 || trait >= TRAIT_COUNT) {
            return false;
        }
        if (index > 0 && trait != -1 && build.traits[index - 1] == -1) {
            return false;
        }
        for (std::size_t previous = 0; previous < index; previous++) {
            if (trait != -1 && build.traits[previous] == trait) {
                return false;
            }
        }
    }
    return build.unspentSkillPoints >= 0
        && build.level >= 1
        && build.level <= kMaximumStoredValue
        && build.experience >= 0;
}

void appendPlayer(std::vector<std::uint8_t>& bytes, const SavedPlayerCharacter& player)
{
    appendUInt32(bytes, player.playerId.value);
    bytes.push_back(static_cast<std::uint8_t>(player.name.size()));
    bytes.insert(bytes.end(), player.name.begin(), player.name.end());
    appendInt32Array(bytes, player.build.baseStats);
    appendInt32Array(bytes, player.build.bonusStats);
    appendInt32Array(bytes, player.build.skillPoints);
    appendInt32Array(bytes, player.build.perkRanks);
    appendInt32Array(bytes, player.build.taggedSkills);
    appendInt32Array(bytes, player.build.traits);
    appendUInt32(bytes, player.build.prototypeFlags);
    appendInt32(bytes, player.build.unspentSkillPoints);
    appendInt32(bytes, player.build.level);
    appendInt32(bytes, player.build.experience);
}

bool readPlayer(PayloadReader& reader, SavedPlayerCharacter& player)
{
    std::uint8_t nameLength;
    if (!reader.readUInt32(player.playerId.value)
        || !reader.readUInt8(nameLength)
        || nameLength > kCharacterNameMaxLength
        || !reader.readString(nameLength, player.name)
        || !readInt32Array(reader, player.build.baseStats)
        || !readInt32Array(reader, player.build.bonusStats)
        || !readInt32Array(reader, player.build.skillPoints)
        || !readInt32Array(reader, player.build.perkRanks)
        || !readInt32Array(reader, player.build.taggedSkills)
        || !readInt32Array(reader, player.build.traits)
        || !reader.readUInt32(player.build.prototypeFlags)
        || !reader.readInt32(player.build.unspentSkillPoints)
        || !reader.readInt32(player.build.level)
        || !reader.readInt32(player.build.experience)) {
        return false;
    }
    return true;
}

} // namespace

std::uint64_t updateMultiplayerSaveDigest(std::uint64_t digest, const void* data, std::size_t size)
{
    const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; index++) {
        digest ^= bytes[index];
        digest *= kFnvPrime;
    }
    return digest;
}

MultiplayerSaveError validateMultiplayerSave(const MultiplayerSaveSidecar& sidecar)
{
    if (sidecar.version < kMultiplayerSaveMinimumVersion
        || sidecar.version > kMultiplayerSaveVersion) {
        return MultiplayerSaveError::UnsupportedVersion;
    }
    if (sidecar.generation == 0) {
        return MultiplayerSaveError::InvalidGeneration;
    }

    bool hostSeen = false;
    bool guestSeen = false;
    for (const SavedPlayerCharacter& player : sidecar.players) {
        if (player.playerId == kHostPlayerId) {
            if (hostSeen) {
                return MultiplayerSaveError::DuplicatePlayer;
            }
            hostSeen = true;
        } else if (player.playerId == kGuestPlayerId) {
            if (guestSeen) {
                return MultiplayerSaveError::DuplicatePlayer;
            }
            guestSeen = true;
        } else {
            return MultiplayerSaveError::InvalidPlayerId;
        }
        if (player.name.empty()) {
            return MultiplayerSaveError::EmptyName;
        }
        if (player.name.size() > kCharacterNameMaxLength) {
            return MultiplayerSaveError::NameTooLong;
        }
        if (!isValidName(player.name)) {
            return MultiplayerSaveError::InvalidName;
        }
        if (!isValidBuild(player.build)) {
            return MultiplayerSaveError::InvalidBuild;
        }
    }
    if (!hostSeen || !guestSeen) {
        return MultiplayerSaveError::PlayerMissing;
    }
    if (sidecar.players[0].playerId != kHostPlayerId
        || sidecar.players[1].playerId != kGuestPlayerId) {
        return MultiplayerSaveError::NonCanonicalPlayerOrder;
    }
    if ((sidecar.version == 1 && !sidecar.guestObjectData.empty())
        || sidecar.guestObjectData.size() > kMultiplayerSaveMaximumSize - kMultiplayerSaveHeaderSize) {
        return MultiplayerSaveError::InvalidGuestObjectData;
    }
    return MultiplayerSaveError::None;
}

MultiplayerSaveError captureMultiplayerSave(const PlayerCharacterStateStore& players,
    std::uint64_t generation,
    std::uint64_t saveDatDigest,
    MultiplayerSaveSidecar& sidecar)
{
    const PlayerCharacterState* host = players.find(kHostPlayerId);
    const PlayerCharacterState* guest = players.find(kGuestPlayerId);
    if (host == nullptr || guest == nullptr) {
        return MultiplayerSaveError::PlayerMissing;
    }

    MultiplayerSaveSidecar captured;
    captured.generation = generation;
    captured.saveDatDigest = saveDatDigest;
    captured.players[0] = { host->id, host->name, host->build };
    captured.players[1] = { guest->id, guest->name, guest->build };
    MultiplayerSaveError error = validateMultiplayerSave(captured);
    if (error == MultiplayerSaveError::None) {
        sidecar = std::move(captured);
    }
    return error;
}

MultiplayerSaveError encodeMultiplayerSave(const MultiplayerSaveSidecar& sidecar, std::vector<std::uint8_t>& bytes)
{
    bytes.clear();
    MultiplayerSaveError error = validateMultiplayerSave(sidecar);
    if (error != MultiplayerSaveError::None) {
        return error;
    }

    std::vector<std::uint8_t> payload;
    appendUInt16(payload, static_cast<std::uint16_t>(kMultiplayerSavePlayerCount));
    appendPlayer(payload, sidecar.players[0]);
    appendPlayer(payload, sidecar.players[1]);
    if (sidecar.version >= 2) {
        appendUInt32(payload, static_cast<std::uint32_t>(sidecar.guestObjectData.size()));
        payload.insert(payload.end(), sidecar.guestObjectData.begin(), sidecar.guestObjectData.end());
    }
    if (payload.size() > kMultiplayerSaveMaximumSize - kMultiplayerSaveHeaderSize) {
        return MultiplayerSaveError::PayloadTooLarge;
    }

    bytes.insert(bytes.end(), std::begin(kMagic), std::end(kMagic));
    appendUInt16(bytes, sidecar.version);
    appendUInt16(bytes, static_cast<std::uint16_t>(kMultiplayerSaveHeaderSize));
    appendUInt64(bytes, sidecar.generation);
    appendUInt64(bytes, sidecar.saveDatDigest);
    appendUInt32(bytes, static_cast<std::uint32_t>(payload.size()));
    appendUInt64(bytes, updateMultiplayerSaveDigest(kMultiplayerSaveDigestOffset, payload.data(), payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return MultiplayerSaveError::None;
}

MultiplayerSaveDecodeResult decodeMultiplayerSave(const std::vector<std::uint8_t>& bytes)
{
    MultiplayerSaveDecodeResult result;
    if (bytes.size() < kMultiplayerSaveHeaderSize) {
        result.error = MultiplayerSaveError::PacketTooShort;
        return result;
    }
    if (!std::equal(std::begin(kMagic), std::end(kMagic), bytes.begin())) {
        result.error = MultiplayerSaveError::InvalidMagic;
        return result;
    }
    result.sidecar.version = readUInt16(bytes, 4);
    if (result.sidecar.version < kMultiplayerSaveMinimumVersion
        || result.sidecar.version > kMultiplayerSaveVersion) {
        result.error = MultiplayerSaveError::UnsupportedVersion;
        return result;
    }
    if (readUInt16(bytes, 6) != kMultiplayerSaveHeaderSize) {
        result.error = MultiplayerSaveError::InvalidHeader;
        return result;
    }
    result.sidecar.generation = readUInt64(bytes, 8);
    result.sidecar.saveDatDigest = readUInt64(bytes, 16);
    std::uint32_t payloadSize = readUInt32(bytes, 24);
    if (payloadSize > kMultiplayerSaveMaximumSize - kMultiplayerSaveHeaderSize) {
        result.error = MultiplayerSaveError::PayloadTooLarge;
        return result;
    }
    std::size_t expectedSize = kMultiplayerSaveHeaderSize + payloadSize;
    if (bytes.size() < expectedSize) {
        result.error = MultiplayerSaveError::TruncatedPayload;
        return result;
    }
    if (bytes.size() > expectedSize) {
        result.error = MultiplayerSaveError::TrailingData;
        return result;
    }
    std::uint64_t expectedChecksum = readUInt64(bytes, 28);
    std::uint64_t actualChecksum = updateMultiplayerSaveDigest(
        kMultiplayerSaveDigestOffset,
        bytes.data() + kMultiplayerSaveHeaderSize,
        payloadSize);
    if (actualChecksum != expectedChecksum) {
        result.error = MultiplayerSaveError::ChecksumMismatch;
        return result;
    }

    PayloadReader reader(bytes, kMultiplayerSaveHeaderSize);
    std::uint16_t playerCount;
    if (!reader.readUInt16(playerCount)) {
        result.error = MultiplayerSaveError::TruncatedPayload;
        return result;
    }
    if (playerCount != kMultiplayerSavePlayerCount) {
        result.error = MultiplayerSaveError::InvalidPlayerCount;
        return result;
    }
    for (SavedPlayerCharacter& player : result.sidecar.players) {
        if (!readPlayer(reader, player)) {
            result.error = MultiplayerSaveError::TruncatedPayload;
            return result;
        }
    }
    if (result.sidecar.version >= 2) {
        std::uint32_t guestObjectSize;
        if (!reader.readUInt32(guestObjectSize)) {
            result.error = MultiplayerSaveError::TruncatedPayload;
            return result;
        }
        if (guestObjectSize > kMultiplayerSaveMaximumSize - kMultiplayerSaveHeaderSize) {
            result.error = MultiplayerSaveError::InvalidGuestObjectData;
            return result;
        }
        if (!reader.readBytes(guestObjectSize, result.sidecar.guestObjectData)) {
            result.error = MultiplayerSaveError::TruncatedPayload;
            return result;
        }
    }
    if (!reader.finished()) {
        result.error = MultiplayerSaveError::TrailingData;
        return result;
    }
    result.error = validateMultiplayerSave(result.sidecar);
    return result;
}

} // namespace multiplayer
} // namespace fallout
