#ifndef FALLOUT_MULTIPLAYER_SNAPSHOT_H_
#define FALLOUT_MULTIPLAYER_SNAPSHOT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "game/worldmap.h"
#include "multiplayer/player_character_state.h"
#include "multiplayer/dialogue_vote_controller.h"
#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

constexpr std::uint32_t kSnapshotMagic = 0x46434D53;
constexpr std::uint16_t kSnapshotVersion = 18;
constexpr std::size_t kSnapshotHeaderSize = 28;
constexpr std::size_t kMaxSnapshotPayloadSize = 512 * 1024;
constexpr std::size_t kMaxSnapshotActors = 16;
constexpr std::size_t kMaxSnapshotCritters = 2048;
constexpr std::size_t kMaxSnapshotDoors = 1024;
constexpr std::size_t kMaxSnapshotScenery = 8192;
constexpr std::size_t kMaxSnapshotItems = 4096;
constexpr std::size_t kMaxSnapshotVariables = 8192;
constexpr std::size_t kMaxSnapshotTimedEvents = 1024;
constexpr std::size_t kMaxTimedEventPayloadValues = 6;
constexpr std::uint32_t kSharedObjectFlagMask = 0xB70FF839;

struct ActorSnapshot {
    EntityId entityId;
    PlayerId ownerId;
    std::int32_t tile = -1;
    std::int32_t elevation = -1;
    std::int32_t rotation = -1;
    std::int32_t hitPoints = 0;
    std::int32_t actionPoints = 0;
    std::int32_t combatResults = 0;
    std::int32_t fid = 0;
    std::int32_t frame = 0;
    std::uint32_t objectFlags = 0;
    std::int32_t lightDistance = 0;
    std::int32_t lightIntensity = 0;
    std::int32_t combatManeuver = 0;
    std::int32_t damageLastTurn = 0;
    std::int32_t team = 0;
    EntityId whoHitMeId;
    CharacterBuild build;
};

struct DoorSnapshot {
    EntityId entityId;
    bool open = false;
    bool locked = false;
    std::int32_t frame = 0;
};

// Shared mutable state for non-door scenery. Local visibility/selection bits
// are deliberately excluded from objectFlags by the engine adapter.
struct ScenerySnapshot {
    EntityId entityId;
    std::int32_t pid = -1;
    std::int32_t fid = -1;
    std::int32_t tile = -1;
    std::int32_t elevation = -1;
    std::int32_t rotation = -1;
    std::int32_t frame = 0;
    std::uint32_t objectFlags = 0;
    std::int32_t lightDistance = 0;
    std::int32_t lightIntensity = 0;
    std::int32_t data0 = 0;
    std::int32_t data1 = 0;
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
    std::int32_t fid = 0;
    std::int32_t frame = 0;
    std::uint32_t objectFlags = 0;
    std::int32_t lightDistance = 0;
    std::int32_t lightIntensity = 0;
    std::int32_t combatManeuver = 0;
    std::int32_t damageLastTurn = 0;
    EntityId whoHitMeId;
};

struct ItemSnapshot {
    EntityId entityId;
    EntityId holderId;
    std::int32_t tile = -1;
    std::int32_t elevation = -1;
    std::uint32_t quantity = 1;
    ItemDescriptor itemDescriptor;
    std::int32_t fid = 0;
    std::int32_t frame = 0;
    std::uint32_t objectFlags = 0;
    std::int32_t lightDistance = 0;
    std::int32_t lightIntensity = 0;
};

struct TimedEventSnapshot {
    std::int32_t time = 0;
    std::uint8_t eventType = 0;
    std::uint8_t payloadCount = 0;
    EntityId ownerId;
    std::array<std::int32_t, kMaxTimedEventPayloadValues> payload {};
};

enum class WorldMapTravelStage : std::uint32_t {
    None = 0,
    Proposed = 1,
    Approved = 2,
};

struct WorldMapTravelSnapshot {
    EntityId proposerActorId;
    EntityId controllerActorId;
    WorldMapTravelStage stage = WorldMapTravelStage::None;
    std::int32_t targetX = -1;
    std::int32_t targetY = -1;
    WorldMapTravelProgress progress;
};

struct WorldSnapshot {
    std::uint16_t version = kSnapshotVersion;
    EventSequence lastIncludedEvent;
    SessionPhase phase = SessionPhase::Lobby;
    std::uint32_t phaseRevision = 0;
    std::int32_t gameTime = 1;
    WorldMapState worldMap;
    WorldMapTravelSnapshot worldMapTravel;
    CombatTurnState combat;
    std::int32_t combatFreeMove = 0;
    std::vector<ActorSnapshot> actors;
    std::vector<CritterSnapshot> critters;
    std::vector<DoorSnapshot> doors;
    std::vector<ScenerySnapshot> scenery;
    std::vector<ItemSnapshot> items;
    std::vector<std::int32_t> gameGlobalVariables;
    std::vector<std::int32_t> mapGlobalVariables;
    std::vector<std::int32_t> mapLocalVariables;
    std::vector<TimedEventSnapshot> timedEvents;
    EntityId dialogueActorId;
    std::optional<DialoguePresentationEvent> dialoguePresentation;
    std::vector<DialogueBallot> dialogueBallots;
    std::vector<SharedActivityEntry> sharedActivity;
    PlayerId nextExtraCapPlayer = kHostPlayerId;
    PlayerId nextItemPriorityPlayer = kHostPlayerId;
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
    InvalidGameTime,
    TooManyActors,
    TooManyCritters,
    TooManyDoors,
    TooManyScenery,
    TooManyItems,
    TooManyVariables,
    TooManyTimedEvents,
    InvalidEntityId,
    InvalidPlayerId,
    InvalidPlayerBuild,
    DuplicateEntityId,
    InvalidActorState,
    InvalidCritterState,
    InvalidDoorState,
    InvalidSceneryState,
    InvalidItemState,
    InvalidTimedEventState,
    InvalidWorldMapState,
    InvalidWorldMapTravelState,
    InvalidCombatState,
    InvalidDialogueState,
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
    Scenery,
    Items,
    Globals,
    MapVariables,
    TimedEvents,
    WorldMap,
};

struct SectionedStateDigest {
    std::uint64_t session = 0;
    std::uint64_t actors = 0;
    std::uint64_t critters = 0;
    std::uint64_t doors = 0;
    std::uint64_t scenery = 0;
    std::uint64_t items = 0;
    std::uint64_t globals = 0;
    std::uint64_t mapVariables = 0;
    std::uint64_t timedEvents = 0;
    std::uint64_t worldMap = 0;
    std::uint64_t overall = 0;
};

constexpr bool operator==(const SectionedStateDigest& lhs, const SectionedStateDigest& rhs)
{
    return lhs.session == rhs.session
        && lhs.actors == rhs.actors
        && lhs.critters == rhs.critters
        && lhs.doors == rhs.doors
        && lhs.scenery == rhs.scenery
        && lhs.items == rhs.items
        && lhs.globals == rhs.globals
        && lhs.mapVariables == rhs.mapVariables
        && lhs.timedEvents == rhs.timedEvents
        && lhs.worldMap == rhs.worldMap
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
