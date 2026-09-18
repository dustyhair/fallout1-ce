#include "multiplayer/snapshot.h"

#include <algorithm>
#include <unordered_set>

namespace fallout {
namespace multiplayer {
namespace {

constexpr std::size_t kSnapshotPayloadHeaderSize = 16;
constexpr std::size_t kActorSnapshotSize = 24;
constexpr std::size_t kDoorSnapshotSize = 12;
constexpr std::size_t kSnapshotProtectedOffset = 20;
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void appendUint8(std::vector<std::uint8_t>& bytes, std::uint8_t value)
{
    bytes.push_back(value);
}

void appendUint16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void appendUint32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void appendUint64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

std::uint8_t readUint8(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
{
    return bytes[offset++];
}

std::uint16_t readUint16(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
{
    std::uint16_t value = static_cast<std::uint16_t>(bytes[offset]) << 8
        | static_cast<std::uint16_t>(bytes[offset + 1]);
    offset += 2;
    return value;
}

std::uint32_t readUint32(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
{
    std::uint32_t value = static_cast<std::uint32_t>(bytes[offset]) << 24
        | static_cast<std::uint32_t>(bytes[offset + 1]) << 16
        | static_cast<std::uint32_t>(bytes[offset + 2]) << 8
        | static_cast<std::uint32_t>(bytes[offset + 3]);
    offset += 4;
    return value;
}

std::uint64_t readUint64(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
{
    std::uint64_t value = 0;
    for (int index = 0; index < 8; index++) {
        value = (value << 8) | bytes[offset++];
    }
    return value;
}

std::uint64_t checksum(const std::uint8_t* data, std::size_t size)
{
    std::uint64_t hash = kFnvOffsetBasis;
    for (std::size_t index = 0; index < size; index++) {
        hash ^= data[index];
        hash *= kFnvPrime;
    }
    return hash;
}

bool isKnownPhase(SessionPhase phase)
{
    return phase >= SessionPhase::Lobby && phase <= SessionPhase::Ending;
}

WorldSnapshot canonicalize(const WorldSnapshot& snapshot)
{
    WorldSnapshot canonical = snapshot;
    std::sort(canonical.actors.begin(), canonical.actors.end(), [](const ActorSnapshot& lhs, const ActorSnapshot& rhs) {
        return lhs.entityId.value < rhs.entityId.value;
    });
    std::sort(canonical.doors.begin(), canonical.doors.end(), [](const DoorSnapshot& lhs, const DoorSnapshot& rhs) {
        return lhs.entityId.value < rhs.entityId.value;
    });
    return canonical;
}

void appendActor(std::vector<std::uint8_t>& bytes, const ActorSnapshot& actor)
{
    appendUint32(bytes, actor.entityId.value);
    appendUint32(bytes, actor.ownerId.value);
    appendUint32(bytes, static_cast<std::uint32_t>(actor.tile));
    appendUint32(bytes, static_cast<std::uint32_t>(actor.elevation));
    appendUint32(bytes, static_cast<std::uint32_t>(actor.rotation));
    appendUint32(bytes, static_cast<std::uint32_t>(actor.hitPoints));
}

void appendDoor(std::vector<std::uint8_t>& bytes, const DoorSnapshot& door)
{
    appendUint32(bytes, door.entityId.value);
    appendUint8(bytes, door.open ? 1 : 0);
    appendUint8(bytes, door.locked ? 1 : 0);
    appendUint16(bytes, 0);
    appendUint32(bytes, static_cast<std::uint32_t>(door.frame));
}

std::uint64_t digestBytes(const std::vector<std::uint8_t>& bytes)
{
    return checksum(bytes.data(), bytes.size());
}

} // namespace

SnapshotError validateSnapshot(const WorldSnapshot& snapshot)
{
    if (snapshot.version != kSnapshotVersion) {
        return SnapshotError::UnsupportedVersion;
    }
    if (!isKnownPhase(snapshot.phase)) {
        return SnapshotError::InvalidPhase;
    }
    if (snapshot.phaseRevision == 0) {
        return SnapshotError::InvalidPhaseRevision;
    }
    if (snapshot.actors.size() > kMaxSnapshotActors) {
        return SnapshotError::TooManyActors;
    }
    if (snapshot.doors.size() > kMaxSnapshotDoors) {
        return SnapshotError::TooManyDoors;
    }

    std::unordered_set<std::uint32_t> entityIds;
    for (const ActorSnapshot& actor : snapshot.actors) {
        if (!isValid(actor.entityId)) {
            return SnapshotError::InvalidEntityId;
        }
        if (!isValid(actor.ownerId)
            || (actor.ownerId != kHostPlayerId && actor.ownerId != kGuestPlayerId)) {
            return SnapshotError::InvalidPlayerId;
        }
        if (!entityIds.insert(actor.entityId.value).second) {
            return SnapshotError::DuplicateEntityId;
        }
        if (actor.tile < 0
            || actor.elevation < 0 || actor.elevation > 2
            || actor.rotation < 0 || actor.rotation > 5
            || actor.hitPoints < 0) {
            return SnapshotError::InvalidActorState;
        }
    }

    for (const DoorSnapshot& door : snapshot.doors) {
        if (!isValid(door.entityId)) {
            return SnapshotError::InvalidEntityId;
        }
        if (!entityIds.insert(door.entityId.value).second) {
            return SnapshotError::DuplicateEntityId;
        }
        if (door.frame < 0) {
            return SnapshotError::InvalidDoorState;
        }
    }

    std::size_t payloadSize = kSnapshotPayloadHeaderSize
        + snapshot.actors.size() * kActorSnapshotSize
        + snapshot.doors.size() * kDoorSnapshotSize;
    if (payloadSize > kMaxSnapshotPayloadSize) {
        return SnapshotError::PayloadTooLarge;
    }

    return SnapshotError::None;
}

SnapshotError encodeSnapshot(const WorldSnapshot& snapshot, std::vector<std::uint8_t>& packet)
{
    packet.clear();
    SnapshotError error = validateSnapshot(snapshot);
    if (error != SnapshotError::None) {
        return error;
    }

    WorldSnapshot canonical = canonicalize(snapshot);
    std::vector<std::uint8_t> payload;
    payload.reserve(kSnapshotPayloadHeaderSize
        + canonical.actors.size() * kActorSnapshotSize
        + canonical.doors.size() * kDoorSnapshotSize);

    appendUint8(payload, static_cast<std::uint8_t>(canonical.phase));
    appendUint8(payload, 0);
    appendUint16(payload, 0);
    appendUint32(payload, canonical.phaseRevision);
    appendUint32(payload, static_cast<std::uint32_t>(canonical.actors.size()));
    appendUint32(payload, static_cast<std::uint32_t>(canonical.doors.size()));
    for (const ActorSnapshot& actor : canonical.actors) {
        appendActor(payload, actor);
    }
    for (const DoorSnapshot& door : canonical.doors) {
        appendDoor(payload, door);
    }

    std::vector<std::uint8_t> protectedBytes;
    protectedBytes.reserve(sizeof(std::uint64_t) + payload.size());
    appendUint64(protectedBytes, canonical.lastIncludedEvent.value);
    protectedBytes.insert(protectedBytes.end(), payload.begin(), payload.end());

    packet.reserve(kSnapshotProtectedOffset + protectedBytes.size());
    appendUint32(packet, kSnapshotMagic);
    appendUint16(packet, canonical.version);
    appendUint16(packet, 0);
    appendUint32(packet, static_cast<std::uint32_t>(payload.size()));
    appendUint64(packet, checksum(protectedBytes.data(), protectedBytes.size()));
    packet.insert(packet.end(), protectedBytes.begin(), protectedBytes.end());
    return SnapshotError::None;
}

SnapshotDecodeResult decodeSnapshot(const std::vector<std::uint8_t>& packet)
{
    SnapshotDecodeResult result;
    if (packet.size() < kSnapshotHeaderSize) {
        result.error = SnapshotError::PacketTooShort;
        return result;
    }

    std::size_t offset = 0;
    if (readUint32(packet, offset) != kSnapshotMagic) {
        result.error = SnapshotError::InvalidMagic;
        return result;
    }

    result.snapshot.version = readUint16(packet, offset);
    if (result.snapshot.version != kSnapshotVersion) {
        result.error = SnapshotError::UnsupportedVersion;
        return result;
    }
    if (readUint16(packet, offset) != 0) {
        result.error = SnapshotError::InvalidReservedField;
        return result;
    }

    std::uint32_t payloadSize = readUint32(packet, offset);
    if (payloadSize > kMaxSnapshotPayloadSize) {
        result.error = SnapshotError::PayloadTooLarge;
        return result;
    }
    std::uint64_t expectedChecksum = readUint64(packet, offset);
    result.snapshot.lastIncludedEvent.value = readUint64(packet, offset);

    std::size_t expectedPacketSize = kSnapshotHeaderSize + payloadSize;
    if (packet.size() < expectedPacketSize) {
        result.error = SnapshotError::TruncatedPayload;
        return result;
    }
    if (packet.size() > expectedPacketSize) {
        result.error = SnapshotError::TrailingData;
        return result;
    }
    if (checksum(packet.data() + kSnapshotProtectedOffset, sizeof(std::uint64_t) + payloadSize) != expectedChecksum) {
        result.error = SnapshotError::ChecksumMismatch;
        return result;
    }
    if (payloadSize < kSnapshotPayloadHeaderSize) {
        result.error = SnapshotError::TruncatedPayload;
        return result;
    }

    offset = kSnapshotHeaderSize;
    result.snapshot.phase = static_cast<SessionPhase>(readUint8(packet, offset));
    if (readUint8(packet, offset) != 0 || readUint16(packet, offset) != 0) {
        result.error = SnapshotError::InvalidReservedField;
        return result;
    }
    result.snapshot.phaseRevision = readUint32(packet, offset);
    std::uint32_t actorCount = readUint32(packet, offset);
    std::uint32_t doorCount = readUint32(packet, offset);

    if (actorCount > kMaxSnapshotActors) {
        result.error = SnapshotError::TooManyActors;
        return result;
    }
    if (doorCount > kMaxSnapshotDoors) {
        result.error = SnapshotError::TooManyDoors;
        return result;
    }

    std::size_t expectedPayloadSize = kSnapshotPayloadHeaderSize
        + static_cast<std::size_t>(actorCount) * kActorSnapshotSize
        + static_cast<std::size_t>(doorCount) * kDoorSnapshotSize;
    if (payloadSize < expectedPayloadSize) {
        result.error = SnapshotError::TruncatedPayload;
        return result;
    }
    if (payloadSize > expectedPayloadSize) {
        result.error = SnapshotError::TrailingData;
        return result;
    }

    result.snapshot.actors.reserve(actorCount);
    for (std::uint32_t index = 0; index < actorCount; index++) {
        ActorSnapshot actor;
        actor.entityId.value = readUint32(packet, offset);
        actor.ownerId.value = readUint32(packet, offset);
        actor.tile = static_cast<std::int32_t>(readUint32(packet, offset));
        actor.elevation = static_cast<std::int32_t>(readUint32(packet, offset));
        actor.rotation = static_cast<std::int32_t>(readUint32(packet, offset));
        actor.hitPoints = static_cast<std::int32_t>(readUint32(packet, offset));
        result.snapshot.actors.push_back(actor);
    }

    result.snapshot.doors.reserve(doorCount);
    for (std::uint32_t index = 0; index < doorCount; index++) {
        DoorSnapshot door;
        door.entityId.value = readUint32(packet, offset);
        std::uint8_t open = readUint8(packet, offset);
        std::uint8_t locked = readUint8(packet, offset);
        if (open > 1 || locked > 1 || readUint16(packet, offset) != 0) {
            result.error = SnapshotError::InvalidDoorState;
            return result;
        }
        door.open = open != 0;
        door.locked = locked != 0;
        door.frame = static_cast<std::int32_t>(readUint32(packet, offset));
        result.snapshot.doors.push_back(door);
    }

    result.error = validateSnapshot(result.snapshot);
    if (result.error == SnapshotError::None) {
        result.snapshot = canonicalize(result.snapshot);
    }
    return result;
}

SnapshotDigestResult computeSnapshotDigest(const WorldSnapshot& snapshot)
{
    SnapshotDigestResult result;
    result.error = validateSnapshot(snapshot);
    if (result.error != SnapshotError::None) {
        return result;
    }

    WorldSnapshot canonical = canonicalize(snapshot);

    std::vector<std::uint8_t> sessionBytes;
    appendUint16(sessionBytes, canonical.version);
    appendUint64(sessionBytes, canonical.lastIncludedEvent.value);
    appendUint8(sessionBytes, static_cast<std::uint8_t>(canonical.phase));
    appendUint32(sessionBytes, canonical.phaseRevision);
    result.digest.session = digestBytes(sessionBytes);

    std::vector<std::uint8_t> actorBytes;
    appendUint32(actorBytes, static_cast<std::uint32_t>(canonical.actors.size()));
    for (const ActorSnapshot& actor : canonical.actors) {
        appendActor(actorBytes, actor);
    }
    result.digest.actors = digestBytes(actorBytes);

    std::vector<std::uint8_t> doorBytes;
    appendUint32(doorBytes, static_cast<std::uint32_t>(canonical.doors.size()));
    for (const DoorSnapshot& door : canonical.doors) {
        appendDoor(doorBytes, door);
    }
    result.digest.doors = digestBytes(doorBytes);

    std::vector<std::uint8_t> overallBytes;
    appendUint64(overallBytes, result.digest.session);
    appendUint64(overallBytes, result.digest.actors);
    appendUint64(overallBytes, result.digest.doors);
    result.digest.overall = digestBytes(overallBytes);
    return result;
}

SnapshotSection firstDivergentSection(const SectionedStateDigest& expected, const SectionedStateDigest& actual)
{
    if (expected.session != actual.session) {
        return SnapshotSection::Session;
    }
    if (expected.actors != actual.actors) {
        return SnapshotSection::Actors;
    }
    if (expected.doors != actual.doors) {
        return SnapshotSection::Doors;
    }
    return SnapshotSection::None;
}

SnapshotError SnapshotReplica::apply(const WorldSnapshot& snapshot)
{
    SnapshotError error = validateSnapshot(snapshot);
    if (error != SnapshotError::None) {
        return error;
    }

    _state = canonicalize(snapshot);
    _hasState = true;
    return SnapshotError::None;
}

bool SnapshotReplica::hasState() const
{
    return _hasState;
}

const WorldSnapshot& SnapshotReplica::state() const
{
    return _state;
}

SnapshotDigestResult SnapshotReplica::digest() const
{
    return computeSnapshotDigest(_state);
}

void SnapshotReplica::clear()
{
    _state = {};
    _hasState = false;
}

} // namespace multiplayer
} // namespace fallout
