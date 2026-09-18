#include "multiplayer/character_lobby.h"

#include <algorithm>
#include <cctype>

#include "game/proto_types.h"

namespace fallout {
namespace multiplayer {
namespace {

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

std::int32_t readInt32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::int32_t>(readUInt32(bytes, offset));
}

bool hasTrait(const CharacterCreationSheet& sheet, int trait)
{
    return std::find(sheet.traits.begin(), sheet.traits.end(), trait) != sheet.traits.end();
}

} // namespace

CharacterLobbyError validateCharacterSheet(const CharacterCreationSheet& sheet)
{
    if (sheet.version != kCharacterSheetVersion) {
        return CharacterLobbyError::UnsupportedVersion;
    }
    if (sheet.playerId != kHostPlayerId && sheet.playerId != kGuestPlayerId) {
        return CharacterLobbyError::InvalidPlayerId;
    }
    if (sheet.name.empty()) {
        return CharacterLobbyError::EmptyName;
    }
    if (sheet.name.size() > kCharacterNameMaxLength) {
        return CharacterLobbyError::NameTooLong;
    }

    bool hasVisibleCharacter = false;
    for (unsigned char ch : sheet.name) {
        if (ch < 32 || ch > 126) {
            return CharacterLobbyError::InvalidName;
        }
        hasVisibleCharacter = hasVisibleCharacter || !std::isspace(ch);
    }
    if (!hasVisibleCharacter) {
        return CharacterLobbyError::InvalidName;
    }

    int primaryStatTotal = 0;
    for (std::int32_t value : sheet.primaryStats) {
        if (value < PRIMARY_STAT_MIN || value > PRIMARY_STAT_MAX) {
            return CharacterLobbyError::PrimaryStatOutOfRange;
        }
        primaryStatTotal += value;
    }
    if (primaryStatTotal != 40) {
        return CharacterLobbyError::InvalidPrimaryStatTotal;
    }

    if (sheet.age < 16 || sheet.age > 35) {
        return CharacterLobbyError::AgeOutOfRange;
    }
    if (sheet.gender != GENDER_MALE && sheet.gender != GENDER_FEMALE) {
        return CharacterLobbyError::InvalidGender;
    }

    for (std::size_t index = 0; index < sheet.taggedSkills.size(); index++) {
        int skill = sheet.taggedSkills[index];
        if (skill < 0 || skill >= SKILL_COUNT) {
            return CharacterLobbyError::InvalidTaggedSkill;
        }
        for (std::size_t previous = 0; previous < index; previous++) {
            if (sheet.taggedSkills[previous] == skill) {
                return CharacterLobbyError::DuplicateTaggedSkill;
            }
        }
    }

    for (std::size_t index = 0; index < sheet.traits.size(); index++) {
        int trait = sheet.traits[index];
        if (trait < -1 || trait >= TRAIT_COUNT) {
            return CharacterLobbyError::InvalidTrait;
        }
        if (index > 0 && trait != -1 && sheet.traits[index - 1] == -1) {
            return CharacterLobbyError::NonCanonicalTraits;
        }
        for (std::size_t previous = 0; previous < index; previous++) {
            if (trait != -1 && sheet.traits[previous] == trait) {
                return CharacterLobbyError::DuplicateTrait;
            }
        }
    }

    return CharacterLobbyError::None;
}

CharacterBuild characterBuildFromSheet(const CharacterCreationSheet& sheet)
{
    CharacterBuild build;
    for (int stat = 0; stat < PRIMARY_STAT_COUNT; stat++) {
        build.baseStats[stat] = sheet.primaryStats[stat];
    }
    build.baseStats[STAT_AGE] = sheet.age;
    build.baseStats[STAT_GENDER] = sheet.gender;
    for (int index = 0; index < DEFAULT_TAGGED_SKILLS; index++) {
        build.taggedSkills[index] = sheet.taggedSkills[index];
    }
    for (int index = 0; index < PC_TRAIT_MAX; index++) {
        build.traits[index] = sheet.traits[index];
    }

    int strength = build.baseStats[STAT_STRENGTH];
    int perception = build.baseStats[STAT_PERCEPTION];
    int endurance = build.baseStats[STAT_ENDURANCE];
    int agility = build.baseStats[STAT_AGILITY];
    int luck = build.baseStats[STAT_LUCK];
    if (hasTrait(sheet, TRAIT_GIFTED)) {
        strength++;
        perception++;
        endurance++;
        agility++;
        luck++;
    }
    if (hasTrait(sheet, TRAIT_BRUISER)) {
        strength += 2;
    }
    if (hasTrait(sheet, TRAIT_SMALL_FRAME)) {
        agility++;
    }

    build.baseStats[STAT_MAXIMUM_HIT_POINTS] = build.baseStats[STAT_STRENGTH] + build.baseStats[STAT_ENDURANCE] * 2 + 15;
    build.baseStats[STAT_MAXIMUM_ACTION_POINTS] = agility / 2 + 5;
    build.baseStats[STAT_ARMOR_CLASS] = agility;
    build.baseStats[STAT_MELEE_DAMAGE] = std::max(strength - 5, 1);
    build.baseStats[STAT_CARRY_WEIGHT] = 25 * strength + 25;
    build.baseStats[STAT_SEQUENCE] = 2 * perception;
    build.baseStats[STAT_HEALING_RATE] = std::max(endurance / 3, 1);
    build.baseStats[STAT_CRITICAL_CHANCE] = luck;
    build.baseStats[STAT_RADIATION_RESISTANCE] = 2 * endurance;
    build.baseStats[STAT_POISON_RESISTANCE] = 5 * endurance;
    build.baseStats[STAT_DAMAGE_RESISTANCE_EMP] = 100;
    return build;
}

CharacterCreationSheet characterSheetFromBuild(PlayerId playerId, const std::string& name, const CharacterBuild& build)
{
    CharacterCreationSheet sheet;
    sheet.playerId = playerId;
    sheet.name = name;
    for (int stat = 0; stat < PRIMARY_STAT_COUNT; stat++) {
        sheet.primaryStats[stat] = build.baseStats[stat];
    }
    sheet.age = build.baseStats[STAT_AGE];
    sheet.gender = build.baseStats[STAT_GENDER];
    for (int index = 0; index < DEFAULT_TAGGED_SKILLS; index++) {
        sheet.taggedSkills[index] = build.taggedSkills[index];
    }
    for (int index = 0; index < PC_TRAIT_MAX; index++) {
        sheet.traits[index] = build.traits[index];
    }
    return sheet;
}

CharacterLobbyError encodeCharacterSheet(const CharacterCreationSheet& sheet, std::vector<std::uint8_t>& packet)
{
    packet.clear();
    CharacterLobbyError error = validateCharacterSheet(sheet);
    if (error != CharacterLobbyError::None) {
        return error;
    }

    packet.reserve(kCharacterSheetMinimumPacketSize + sheet.name.size());
    appendUInt16(packet, sheet.version);
    appendUInt32(packet, sheet.playerId.value);
    packet.push_back(static_cast<std::uint8_t>(sheet.name.size()));
    packet.insert(packet.end(), sheet.name.begin(), sheet.name.end());
    for (std::int32_t stat : sheet.primaryStats) {
        appendInt32(packet, stat);
    }
    appendInt32(packet, sheet.age);
    appendInt32(packet, sheet.gender);
    for (std::int32_t skill : sheet.taggedSkills) {
        appendInt32(packet, skill);
    }
    for (std::int32_t trait : sheet.traits) {
        appendInt32(packet, trait);
    }
    return CharacterLobbyError::None;
}

CharacterSheetDecodeResult decodeCharacterSheet(const std::vector<std::uint8_t>& packet)
{
    CharacterSheetDecodeResult result;
    if (packet.size() < 7) {
        result.error = CharacterLobbyError::PacketTooShort;
        return result;
    }

    result.sheet.version = readUInt16(packet, 0);
    if (result.sheet.version != kCharacterSheetVersion) {
        result.error = CharacterLobbyError::UnsupportedVersion;
        return result;
    }
    result.sheet.playerId.value = readUInt32(packet, 2);

    std::size_t nameLength = packet[6];
    if (nameLength > kCharacterNameMaxLength) {
        result.error = CharacterLobbyError::NameTooLong;
        return result;
    }
    std::size_t expectedSize = kCharacterSheetMinimumPacketSize + nameLength;
    if (packet.size() < expectedSize) {
        result.error = CharacterLobbyError::TruncatedPacket;
        return result;
    }
    if (packet.size() > expectedSize) {
        result.error = CharacterLobbyError::TrailingData;
        return result;
    }

    std::size_t offset = 7;
    result.sheet.name.assign(packet.begin() + offset, packet.begin() + offset + nameLength);
    offset += nameLength;
    for (std::int32_t& stat : result.sheet.primaryStats) {
        stat = readInt32(packet, offset);
        offset += 4;
    }
    result.sheet.age = readInt32(packet, offset);
    offset += 4;
    result.sheet.gender = readInt32(packet, offset);
    offset += 4;
    for (std::int32_t& skill : result.sheet.taggedSkills) {
        skill = readInt32(packet, offset);
        offset += 4;
    }
    for (std::int32_t& trait : result.sheet.traits) {
        trait = readInt32(packet, offset);
        offset += 4;
    }

    result.error = validateCharacterSheet(result.sheet);
    return result;
}

CharacterLobbyError CharacterLobby::submit(const CharacterCreationSheet& submittedSheet, PlayerCharacterStateStore& players)
{
    CharacterLobbyError error = validateCharacterSheet(submittedSheet);
    if (error != CharacterLobbyError::None) {
        return error;
    }

    PlayerCharacterState* player = players.find(submittedSheet.playerId);
    if (player == nullptr) {
        return CharacterLobbyError::PlayerMissing;
    }

    player->name = submittedSheet.name;
    player->build = characterBuildFromSheet(submittedSheet);
    if (submittedSheet.playerId == kHostPlayerId) {
        _hostSheet = submittedSheet;
    } else {
        _guestSheet = submittedSheet;
    }
    return CharacterLobbyError::None;
}

bool CharacterLobby::isReady(PlayerId playerId) const
{
    if (playerId == kHostPlayerId) {
        return _hostSheet.has_value();
    }
    if (playerId == kGuestPlayerId) {
        return _guestSheet.has_value();
    }
    return false;
}

bool CharacterLobby::allPlayersReady() const
{
    return _hostSheet.has_value() && _guestSheet.has_value();
}

const CharacterCreationSheet* CharacterLobby::sheet(PlayerId playerId) const
{
    if (playerId == kHostPlayerId) {
        return _hostSheet.has_value() ? &*_hostSheet : nullptr;
    }
    if (playerId == kGuestPlayerId) {
        return _guestSheet.has_value() ? &*_guestSheet : nullptr;
    }
    return nullptr;
}

void CharacterLobby::reset()
{
    _hostSheet.reset();
    _guestSheet.reset();
}

} // namespace multiplayer
} // namespace fallout
