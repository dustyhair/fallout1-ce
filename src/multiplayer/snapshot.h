#ifndef FALLOUT_MULTIPLAYER_SNAPSHOT_H_
#define FALLOUT_MULTIPLAYER_SNAPSHOT_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

constexpr std::uint32_t kSnapshotMagic = 0x46434D53;
constexpr std::uint16_t kSnapshotVersion = 4;
constexpr std::size_t kSnapshotHeaderSize = 28;
constexpr std::size_t kMaxSnapshotPayloadSize = 64 * 1024;
constexpr std::size_t kMaxSnapshotActors = 16;
constexpr std::size_t kMaxSnapshotCritters = 2048;
constexpr std::size_t kMaxSnapshotDoors = 1024;
constexpr std::size_t kMaxSnapshotItems = 4096;

struct ActorSnapshot {
    EntityId entityId;
    PlayerId ownerId;
    std::int32_t tile = -1;
    std::int32_t elevation = -1;
    std::int32_t rotation = -1;
    std::int32_t hitPoints = 0;
    std::int32_t actionPoints = 0;
    std::int32_t combatResults = 0;
};

struct DoorSnapshot {
    EntityId entityId;
    bool open = false;
    bool locked = false;
    std::int32_t frame = 0;
};

struct CritterSnapshot {
    EntityId entityId;
    std::int32_t pid = -1;
    std::int32_t tile = -1;
    std::int32_t elevation = -1;
    std::int32_t rotation = -1;
    std::int32_t hitPoints = 0;
    std::int32_t actionPoints = 0;
    std::int32_t combatResults = 0;
    std::int32_t team = 0;
};

struct ItemSnapshot {
    EntityId entityId;
    EntityId holderId;
    std::int32_t tile = -1;
    std::int32_t elevation = -1;
    std::uint32_t quantity = 1;
    ItemDescriptor itemDescriptor;
};

struct WorldSnapshot {
    std::uint16_t version = kSnapshotVersion;
    EventSequence lastIncludedEvent;
    SessionPhase phase = SessionPhase::Lobby;
    std::uint32_t phaseRevision = 0;
    std::vector<ActorSnapshot> actors;
    std::vector<CritterSnapshot> critters;
    std::vector<DoorSnapshot> doors;
    std::vector<ItemSnapshot> items;
};

enum class SnapshotError {
    None,
    PacketTooShort,
    InvalidMagic,
    UnsupportedVersion,
    PayloadTooLarge,
    TruncatedPayload,
    TrailingData,
    ChecksumMismatch,
    InvalidReservedField,
    InvalidPhase,
    InvalidPhaseRevision,
    TooManyActors,
    TooManyCritters,
    TooManyDoors,
    TooManyItems,
    InvalidEntityId,
    InvalidPlayerId,
    DuplicateEntityId,
    InvalidActorState,
    InvalidCritterState,
    InvalidDoorState,
    InvalidItemState,
};

struct SnapshotDecodeResult {
    SnapshotError error = SnapshotError::None;
    WorldSnapshot snapshot;

    explicit operator bool() const
    {
        return error == SnapshotError::None;
    }
};

enum class SnapshotSection {
    None,
    Session,
    Actors,
    Critters,
    Doors,
    Items,
};

struct SectionedStateDigest {
    std::uint64_t session = 0;
    std::uint64_t actors = 0;
    std::uint64_t critters = 0;
    std::uint64_t doors = 0;
    std::uint64_t items = 0;
    std::uint64_t overall = 0;
};

constexpr bool operator==(const SectionedStateDigest& lhs, const SectionedStateDigest& rhs)
{
    return lhs.session == rhs.session
        && lhs.actors == rhs.actors
        && lhs.critters == rhs.critters
        && lhs.doors == rhs.doors
        && lhs.items == rhs.items
        && lhs.overall == rhs.overall;
}

constexpr bool operator!=(const SectionedStateDigest& lhs, const SectionedStateDigest& rhs)
{
    return !(lhs == rhs);
}

struct SnapshotDigestResult {
    SnapshotError error = SnapshotError::None;
    SectionedStateDigest digest;

    explicit operator bool() const
    {
        return error == SnapshotError::None;
    }
};

SnapshotError validateSnapshot(const WorldSnapshot& snapshot);
SnapshotError encodeSnapshot(const WorldSnapshot& snapshot, std::vector<std::uint8_t>& packet);
SnapshotDecodeResult decodeSnapshot(const std::vector<std::uint8_t>& packet);
SnapshotDigestResult computeSnapshotDigest(const WorldSnapshot& snapshot);
SnapshotSection firstDivergentSection(const SectionedStateDigest& expected, const SectionedStateDigest& actual);

class SnapshotReplica {
public:
    SnapshotError apply(const WorldSnapshot& snapshot);
    bool hasState() const;
    const WorldSnapshot& state() const;
    SnapshotDigestResult digest() const;
    void clear();

private:
    bool _hasState = false;
    WorldSnapshot _state;
};

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_SNAPSHOT_H_ */
