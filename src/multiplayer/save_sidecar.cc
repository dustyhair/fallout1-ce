#include "multiplayer/save_sidecar.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>
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

    bool readUInt64(std::uint64_t& value)
    {
        std::uint32_t high;
        std::uint32_t low;
        if (!readUInt32(high) || !readUInt32(low)) return false;
        value = (static_cast<std::uint64_t>(high) << 32) | low;
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

void appendPlayerBase(std::vector<std::uint8_t>& bytes, const SavedPlayerCharacter& player)
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

bool readPlayerBase(PayloadReader& reader, SavedPlayerCharacter& player)
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

void appendPlayerV3(std::vector<std::uint8_t>& bytes,
    const SavedPlayerCharacter& player)
{
    appendPlayerBase(bytes, player);
    appendUInt32(bytes, player.actorId.value);
    bytes.push_back(player.replacementAllowed ? 1 : 0);
    bytes.push_back(0);
    appendUInt16(bytes, 0);
    bytes.insert(bytes.end(), player.reconnectToken.bytes.begin(),
        player.reconnectToken.bytes.end());
    appendUInt32(bytes, static_cast<std::uint32_t>(player.objectData.size()));
    bytes.insert(bytes.end(), player.objectData.begin(), player.objectData.end());
}

bool readPlayerV3(PayloadReader& reader, SavedPlayerCharacter& player)
{
    std::uint8_t replacementAllowed;
    std::uint8_t reserved8;
    std::uint16_t reserved16;
    std::uint32_t objectSize;
    if (!readPlayerBase(reader, player)
        || !reader.readUInt32(player.actorId.value)
        || !reader.readUInt8(replacementAllowed)
        || !reader.readUInt8(reserved8)
        || !reader.readUInt16(reserved16)
        || replacementAllowed > 1 || reserved8 != 0 || reserved16 != 0) {
        return false;
    }
    for (std::uint8_t& byte : player.reconnectToken.bytes) {
        if (!reader.readUInt8(byte)) return false;
    }
    if (!reader.readUInt32(objectSize)
        || objectSize > kMultiplayerSaveMaximumSize - kMultiplayerSaveHeaderSize
        || !reader.readBytes(objectSize, player.objectData)) return false;
    player.replacementAllowed = replacementAllowed != 0;
    return true;
}

void appendSharedActivity(std::vector<std::uint8_t>& bytes,
    const SharedActivityEntry& entry)
{
    appendUInt64(bytes, entry.id);
    appendUInt32(bytes, entry.sourceId.value);
    bytes.push_back(static_cast<std::uint8_t>(entry.kind));
    bytes.push_back(static_cast<std::uint8_t>(entry.sourceName.size()));
    appendUInt16(bytes, static_cast<std::uint16_t>(entry.text.size()));
    appendInt32(bytes, entry.subject);
    appendInt32(bytes, entry.value);
    bytes.insert(bytes.end(), entry.sourceName.begin(), entry.sourceName.end());
    bytes.insert(bytes.end(), entry.text.begin(), entry.text.end());
}

bool readSharedActivity(PayloadReader& reader, SharedActivityEntry& entry)
{
    std::uint8_t kind;
    std::uint8_t nameLength;
    std::uint16_t textLength;
    if (!reader.readUInt64(entry.id)
        || !reader.readUInt32(entry.sourceId.value)
        || !reader.readUInt8(kind)
        || !reader.readUInt8(nameLength)
        || !reader.readUInt16(textLength)
        || !reader.readInt32(entry.subject)
        || !reader.readInt32(entry.value)
        || nameLength == 0 || nameLength > 32
        || textLength == 0 || textLength > 160
        || !reader.readString(nameLength, entry.sourceName)
        || !reader.readString(textLength, entry.text)) {
        return false;
    }
    entry.kind = static_cast<SharedActivityKind>(kind);
    return true;
}

const SavedPlayerCharacter* findPlayer(const MultiplayerSaveSidecar& sidecar,
    PlayerId playerId)
{
    auto found = std::find_if(sidecar.players.begin(), sidecar.players.end(),
        [playerId](const SavedPlayerCharacter& player) {
            return player.playerId == playerId;
        });
    return found != sidecar.players.end() ? &*found : nullptr;
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

    if ((sidecar.version < 3
            && sidecar.players.size() != kMultiplayerSaveLegacyPlayerCount)
        || (sidecar.version >= 3
            && (sidecar.players.size() < kMultiplayerSaveLegacyPlayerCount
                || sidecar.players.size() > kMultiplayerSaveMaximumPlayers))) {
        return MultiplayerSaveError::InvalidPlayerCount;
    }

    bool hostSeen = false;
    bool guestSeen = false;
    std::size_t objectDataSize = 0;
    std::unordered_set<EntityId, EntityIdHash> actorIds;
    std::vector<ReconnectToken> reconnectTokens;
    PlayerId previousPlayerId;
    for (const SavedPlayerCharacter& player : sidecar.players) {
        if (!isValid(player.playerId)) return MultiplayerSaveError::InvalidPlayerId;
        if (isValid(previousPlayerId)
            && player.playerId.value <= previousPlayerId.value) {
            return player.playerId == previousPlayerId
                ? MultiplayerSaveError::DuplicatePlayer
                : MultiplayerSaveError::NonCanonicalPlayerOrder;
        }
        previousPlayerId = player.playerId;
        hostSeen = hostSeen || player.playerId == kHostPlayerId;
        guestSeen = guestSeen || player.playerId == kGuestPlayerId;
        if (!isValid(player.actorId)
            || !actorIds.insert(player.actorId).second) {
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
        if (player.objectData.size()
                > kMultiplayerSaveMaximumSize - kMultiplayerSaveHeaderSize
            || objectDataSize
                > kMultiplayerSaveMaximumSize - kMultiplayerSaveHeaderSize
                    - player.objectData.size()) {
            return MultiplayerSaveError::InvalidPlayerObjectData;
        }
        objectDataSize += player.objectData.size();
        bool hasToken = isValid(player.reconnectToken);
        bool emptyToken = std::all_of(player.reconnectToken.bytes.begin(),
            player.reconnectToken.bytes.end(), [](std::uint8_t byte) {
                return byte == 0;
            });
        if (!hasToken && !emptyToken) {
            return MultiplayerSaveError::InvalidReconnectToken;
        }
        if (hasToken) {
            if (std::any_of(reconnectTokens.begin(), reconnectTokens.end(),
                    [&](const ReconnectToken& token) {
                        return reconnectTokensEqual(token,
                            player.reconnectToken);
                    })) {
                return MultiplayerSaveError::InvalidReconnectToken;
            }
            reconnectTokens.push_back(player.reconnectToken);
        }
        if (player.playerId == kHostPlayerId
            && (player.replacementAllowed || !player.objectData.empty())) {
            return MultiplayerSaveError::InvalidPlayerObjectData;
        }
    }
    if (!hostSeen || !guestSeen) {
        return MultiplayerSaveError::PlayerMissing;
    }
    if (sidecar.players.front().playerId != kHostPlayerId) {
        return MultiplayerSaveError::NonCanonicalPlayerOrder;
    }
    if (sidecar.version < 3) {
        for (const SavedPlayerCharacter& player : sidecar.players) {
            if ((sidecar.version == 1 && !player.objectData.empty())
                || (player.playerId != kGuestPlayerId
                    && !player.objectData.empty())) {
                return MultiplayerSaveError::InvalidPlayerObjectData;
            }
            if (player.actorId.value != player.playerId.value
                || isValid(player.reconnectToken)) {
                return MultiplayerSaveError::InvalidHeader;
            }
        }
        if (sidecar.sessionRules != 0) {
            return MultiplayerSaveError::InvalidHeader;
        }
        if (!sidecar.lootDistribution.roster.empty()) {
            if (validateLootDistributionState(sidecar.lootDistribution)
                    != LootDistributionError::None
                || sidecar.lootDistribution.roster.size()
                    != sidecar.players.size()) {
                return MultiplayerSaveError::InvalidLootState;
            }
            for (std::size_t index = 0; index < sidecar.players.size(); index++) {
                if (sidecar.lootDistribution.roster[index]
                    != sidecar.players[index].playerId) {
                    return MultiplayerSaveError::InvalidLootState;
                }
            }
        }
        if (!sidecar.ownership.empty()) {
            if (sidecar.ownership.size() != sidecar.players.size()) {
                return MultiplayerSaveError::InvalidOwnership;
            }
            for (std::size_t index = 0; index < sidecar.players.size(); index++) {
                if (sidecar.ownership[index].entityId
                        != sidecar.players[index].actorId
                    || sidecar.ownership[index].ownerId
                        != sidecar.players[index].playerId) {
                    return MultiplayerSaveError::InvalidOwnership;
                }
            }
        }
        return sidecar.sharedActivity.empty()
            ? MultiplayerSaveError::None
            : MultiplayerSaveError::InvalidActivity;
    }

    if (validateLootDistributionState(sidecar.lootDistribution)
            != LootDistributionError::None
        || sidecar.lootDistribution.roster.size() != sidecar.players.size()) {
        return MultiplayerSaveError::InvalidLootState;
    }
    for (std::size_t index = 0; index < sidecar.players.size(); index++) {
        if (sidecar.lootDistribution.roster[index]
            != sidecar.players[index].playerId) {
            return MultiplayerSaveError::InvalidLootState;
        }
    }
    if (sidecar.ownership.size() > kMultiplayerSaveMaximumOwnershipRecords) {
        return MultiplayerSaveError::InvalidOwnership;
    }
    EntityId previousEntityId;
    for (const SavedEntityOwnership& ownership : sidecar.ownership) {
        if (!isValid(ownership.entityId) || !isValid(ownership.ownerId)
            || (isValid(previousEntityId)
                && ownership.entityId.value <= previousEntityId.value)
            || findPlayer(sidecar, ownership.ownerId) == nullptr) {
            return MultiplayerSaveError::InvalidOwnership;
        }
        previousEntityId = ownership.entityId;
    }
    if (sidecar.version < 4) {
        if (!sidecar.sharedActivity.empty()) {
            return MultiplayerSaveError::InvalidActivity;
        }
        return MultiplayerSaveError::None;
    }
    if (sidecar.sharedActivity.size()
        > kMultiplayerSaveMaximumActivityEntries) {
        return MultiplayerSaveError::InvalidActivity;
    }
    std::uint64_t previousActivityId = 0;
    for (const SharedActivityEntry& entry : sidecar.sharedActivity) {
        bool sourceKnown = entry.sourceId.value == 0
            || findPlayer(sidecar, entry.sourceId) != nullptr;
        bool printable = std::all_of(entry.text.begin(), entry.text.end(),
            [](unsigned char ch) { return ch >= 32 && ch <= 126; });
        if (entry.id <= previousActivityId || !sourceKnown
            || entry.kind < SharedActivityKind::Quest
            || entry.kind > SharedActivityKind::WorldOutcome
            || entry.sourceName.empty() || entry.sourceName.size() > 32
            || !isValidName(entry.sourceName)
            || entry.text.empty() || entry.text.size() > 160
            || !printable) {
            return MultiplayerSaveError::InvalidActivity;
        }
        previousActivityId = entry.id;
    }
    return MultiplayerSaveError::None;
}

MultiplayerSaveError captureMultiplayerSave(const PlayerCharacterStateStore& players,
    std::uint64_t generation,
    std::uint64_t saveDatDigest,
    MultiplayerSaveSidecar& sidecar)
{
    std::vector<PlayerId> playerIds = players.playerIds();
    if (playerIds.size() < kMultiplayerSaveLegacyPlayerCount
        || playerIds.size() > kMultiplayerSaveMaximumPlayers
        || players.find(kHostPlayerId) == nullptr
        || players.find(kGuestPlayerId) == nullptr) {
        return MultiplayerSaveError::PlayerMissing;
    }

    MultiplayerSaveSidecar captured;
    captured.generation = generation;
    captured.saveDatDigest = saveDatDigest;
    for (PlayerId playerId : playerIds) {
        const PlayerCharacterState* player = players.find(playerId);
        captured.players.push_back(SavedPlayerCharacter {
            player->id,
            player->actorId,
            player->name,
            player->build,
            {},
            {},
            player->id != kHostPlayerId,
        });
        captured.ownership.push_back(
            SavedEntityOwnership { player->actorId, player->id });
    }
    LootDistributionController loot;
    if (loot.begin(playerIds) != LootDistributionError::None) {
        return MultiplayerSaveError::InvalidLootState;
    }
    captured.lootDistribution = loot.state();
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
    appendUInt16(payload, static_cast<std::uint16_t>(sidecar.players.size()));
    if (sidecar.version < 3) {
        for (const SavedPlayerCharacter& player : sidecar.players) {
            appendPlayerBase(payload, player);
        }
        if (sidecar.version >= 2) {
            const SavedPlayerCharacter* guest = findPlayer(sidecar, kGuestPlayerId);
            appendUInt32(payload, static_cast<std::uint32_t>(guest->objectData.size()));
            payload.insert(payload.end(), guest->objectData.begin(),
                guest->objectData.end());
        }
    } else {
        for (const SavedPlayerCharacter& player : sidecar.players) {
            appendPlayerV3(payload, player);
        }
        appendUInt32(payload, sidecar.sessionRules);
        appendUInt16(payload,
            static_cast<std::uint16_t>(sidecar.lootDistribution.roster.size()));
        appendUInt16(payload, 0);
        appendUInt32(payload, sidecar.lootDistribution.nextCapExtraIndex);
        appendUInt32(payload, sidecar.lootDistribution.nextLootPriorityIndex);
        for (PlayerId playerId : sidecar.lootDistribution.roster) {
            appendUInt32(payload, playerId.value);
        }
        appendUInt32(payload, static_cast<std::uint32_t>(sidecar.ownership.size()));
        for (const SavedEntityOwnership& ownership : sidecar.ownership) {
            appendUInt32(payload, ownership.entityId.value);
            appendUInt32(payload, ownership.ownerId.value);
        }
        if (sidecar.version >= 4) {
            appendUInt32(payload,
                static_cast<std::uint32_t>(sidecar.sharedActivity.size()));
            for (const SharedActivityEntry& entry : sidecar.sharedActivity) {
                appendSharedActivity(payload, entry);
            }
        }
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
    if ((result.sidecar.version < 3
            && playerCount != kMultiplayerSaveLegacyPlayerCount)
        || (result.sidecar.version >= 3
            && (playerCount < kMultiplayerSaveLegacyPlayerCount
                || playerCount > kMultiplayerSaveMaximumPlayers))) {
        result.error = MultiplayerSaveError::InvalidPlayerCount;
        return result;
    }
    result.sidecar.players.resize(playerCount);
    for (SavedPlayerCharacter& player : result.sidecar.players) {
        bool read = result.sidecar.version >= 3
            ? readPlayerV3(reader, player)
            : readPlayerBase(reader, player);
        if (!read) {
            result.error = MultiplayerSaveError::TruncatedPayload;
            return result;
        }
        if (result.sidecar.version < 3) {
            player.actorId = EntityId { player.playerId.value };
            player.replacementAllowed = player.playerId != kHostPlayerId;
        }
    }
    if (result.sidecar.version == 2) {
        std::uint32_t guestObjectSize;
        if (!reader.readUInt32(guestObjectSize)) {
            result.error = MultiplayerSaveError::TruncatedPayload;
            return result;
        }
        if (guestObjectSize > kMultiplayerSaveMaximumSize - kMultiplayerSaveHeaderSize) {
            result.error = MultiplayerSaveError::InvalidPlayerObjectData;
            return result;
        }
        SavedPlayerCharacter* guest = nullptr;
        for (SavedPlayerCharacter& player : result.sidecar.players) {
            if (player.playerId == kGuestPlayerId) guest = &player;
        }
        if (guest == nullptr
            || !reader.readBytes(guestObjectSize, guest->objectData)) {
            result.error = MultiplayerSaveError::TruncatedPayload;
            return result;
        }
    }
    if (result.sidecar.version < 3) {
        LootDistributionController loot;
        std::vector<PlayerId> playerIds;
        for (const SavedPlayerCharacter& player : result.sidecar.players) {
            playerIds.push_back(player.playerId);
            result.sidecar.ownership.push_back(
                SavedEntityOwnership { player.actorId, player.playerId });
        }
        loot.begin(playerIds);
        result.sidecar.lootDistribution = loot.state();
    } else {
        std::uint16_t lootPlayerCount;
        std::uint16_t reserved;
        std::uint32_t ownershipCount;
        if (!reader.readUInt32(result.sidecar.sessionRules)
            || !reader.readUInt16(lootPlayerCount)
            || !reader.readUInt16(reserved)
            || reserved != 0
            || lootPlayerCount > kMultiplayerSaveMaximumPlayers
            || !reader.readUInt32(
                result.sidecar.lootDistribution.nextCapExtraIndex)
            || !reader.readUInt32(
                result.sidecar.lootDistribution.nextLootPriorityIndex)) {
            result.error = MultiplayerSaveError::TruncatedPayload;
            return result;
        }
        result.sidecar.lootDistribution.roster.resize(lootPlayerCount);
        for (PlayerId& playerId : result.sidecar.lootDistribution.roster) {
            if (!reader.readUInt32(playerId.value)) {
                result.error = MultiplayerSaveError::TruncatedPayload;
                return result;
            }
        }
        if (!reader.readUInt32(ownershipCount)
            || ownershipCount > kMultiplayerSaveMaximumOwnershipRecords) {
            result.error = MultiplayerSaveError::InvalidOwnership;
            return result;
        }
        result.sidecar.ownership.resize(ownershipCount);
        for (SavedEntityOwnership& ownership : result.sidecar.ownership) {
            if (!reader.readUInt32(ownership.entityId.value)
                || !reader.readUInt32(ownership.ownerId.value)) {
                result.error = MultiplayerSaveError::TruncatedPayload;
                return result;
            }
        }
        if (result.sidecar.version >= 4) {
            std::uint32_t activityCount;
            if (!reader.readUInt32(activityCount)
                || activityCount > kMultiplayerSaveMaximumActivityEntries) {
                result.error = MultiplayerSaveError::InvalidActivity;
                return result;
            }
            result.sidecar.sharedActivity.resize(activityCount);
            for (SharedActivityEntry& entry : result.sidecar.sharedActivity) {
                if (!readSharedActivity(reader, entry)) {
                    result.error = MultiplayerSaveError::TruncatedPayload;
                    return result;
                }
            }
        }
    }
    if (!reader.finished()) {
        result.error = MultiplayerSaveError::TrailingData;
        return result;
    }
    result.error = validateMultiplayerSave(result.sidecar);
    return result;
}

SavedPlayerSlotResolution resolveSavedPlayerSlot(
    const MultiplayerSaveSidecar& sidecar,
    PlayerId playerId,
    const ReconnectToken& reconnectToken,
    bool playerPresent)
{
    if (validateMultiplayerSave(sidecar) != MultiplayerSaveError::None) {
        return SavedPlayerSlotResolution::Rejected;
    }
    const SavedPlayerCharacter* player = findPlayer(sidecar, playerId);
    if (player == nullptr) return SavedPlayerSlotResolution::Rejected;
    if (!playerPresent) {
        return player->replacementAllowed
            ? SavedPlayerSlotResolution::MissingAllowed
            : SavedPlayerSlotResolution::Rejected;
    }
    if (isValid(player->reconnectToken)
        && reconnectTokensEqual(player->reconnectToken, reconnectToken)) {
        return SavedPlayerSlotResolution::ExactReconnect;
    }
    return player->replacementAllowed
        ? SavedPlayerSlotResolution::ReplacementAllowed
        : SavedPlayerSlotResolution::Rejected;
}

MultiplayerSaveError claimSavedPlayerSlot(MultiplayerSaveSidecar& sidecar,
    PlayerId playerId,
    const ReconnectToken& replacementToken)
{
    MultiplayerSaveError error = validateMultiplayerSave(sidecar);
    if (error != MultiplayerSaveError::None) return error;
    auto found = std::find_if(sidecar.players.begin(), sidecar.players.end(),
        [playerId](const SavedPlayerCharacter& player) {
            return player.playerId == playerId;
        });
    if (found == sidecar.players.end()) {
        return MultiplayerSaveError::PlayerMissing;
    }
    if (!found->replacementAllowed || !isValid(replacementToken)) {
        return MultiplayerSaveError::InvalidReconnectToken;
    }
    ReconnectToken previous = found->reconnectToken;
    found->reconnectToken = replacementToken;
    error = validateMultiplayerSave(sidecar);
    if (error != MultiplayerSaveError::None) found->reconnectToken = previous;
    return error;
}

} // namespace multiplayer
} // namespace fallout
