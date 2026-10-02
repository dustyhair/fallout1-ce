#include "game/protinst.h"
#include "native_snapshot_creation_support.h"

namespace fallout {
namespace {
int snapshotPlacementAttempts = 0;
int nativeSnapshotAttemptPlacement(Object* object, int tile, int elevation, int radius)
{
    ++snapshotPlacementAttempts;
    return obj_attempt_placement(object, tile, elevation, radius);
}
} // namespace
} // namespace fallout

// Compile the actual world bridge with a private wrapper. Runtime and the
// desktop entry point remain the normal game objects in the linked fixture.
#define obj_attempt_placement nativeSnapshotAttemptPlacement
#define networkWorldRunEngineAuthoritySmokeTest nativeSnapshotOriginalAuthoritySmokeTest
#include "multiplayer/network_world.cc"
#undef networkWorldRunEngineAuthoritySmokeTest
#undef obj_attempt_placement

namespace fallout {
namespace multiplayer {
namespace {

bool sameQueue(const std::vector<QueueEventState>& left, const std::vector<QueueEventState>& right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto& before = left[index];
        const auto& after = right[index];
        if (before.time != after.time || before.eventType != after.eventType
            || before.owner != after.owner || before.payloadCount != after.payloadCount
            || before.payload != after.payload) return false;
    }
    return true;
}

bool runNativeSnapshotCreationControl()
{
    WorldSnapshot baseline;
    if (!networkWorldCaptureSnapshot({}, baseline)
        || baseline.scenery.empty() || baseline.critters.empty() || baseline.actors.empty()) return false;
    auto baselineDigest = computeSnapshotDigest(baseline);
    if (!baselineDigest) return false;

    std::uint32_t nextId = 1;
    auto includeIds = [&nextId](const auto& entries) {
        for (const auto& state : entries) nextId = std::max(nextId, state.entityId.value + 1);
    };
    includeIds(baseline.actors);
    includeIds(baseline.doors);
    includeIds(baseline.scenery);
    includeIds(baseline.critters);
    includeIds(baseline.items);
    while (session.entities().contains(EntityId { nextId })) ++nextId;
    WorldSnapshot control = baseline;
    ScenerySnapshot scenery = control.scenery.front();
    scenery.entityId = EntityId { nextId++ };
    scenery.tile = 39990;
    scenery.frame = 0;
    for (const auto& state : baseline.scenery) {
        if (state.pid == scenery.pid && state.tile == scenery.tile && state.elevation == scenery.elevation) return false;
    }
    control.scenery.push_back(scenery);
    CritterSnapshot npc = control.critters.front();
    npc.entityId = EntityId { nextId++ };
    npc.tile = baseline.actors.front().tile;
    npc.elevation = baseline.actors.front().elevation;
    npc.frame = 0;
    npc.whoHitMeId = {};
    control.critters.push_back(npc);
    Proto* bagPrototype = nullptr;
    Proto* childPrototype = nullptr;
    if (proto_ptr(211, &bagPrototype) != 0 || proto_ptr(PROTO_ID_STIMPACK, &childPrototype) != 0) return false;
    ItemSnapshot bag;
    bag.entityId = EntityId { nextId++ };
    bag.itemDescriptor.pid = 211;
    bag.fid = bagPrototype->fid;
    bag.tile = 39989;
    bag.elevation = 0;
    bag.quantity = 1;
    control.items.push_back(bag);
    ItemSnapshot child;
    child.entityId = EntityId { nextId++ };
    child.itemDescriptor.pid = PROTO_ID_STIMPACK;
    child.fid = childPrototype->fid;
    child.holderId = bag.entityId;
    child.tile = -1;
    child.elevation = -1;
    child.quantity = 1;
    control.items.push_back(child);
    ++control.actors.front().hitPoints;
    control.mapLocalVariables.push_back(345678);
    std::vector<std::uint8_t> packet;
    if (encodeSnapshot(control, packet) != SnapshotError::None) return false;
    auto decoded = decodeSnapshot(packet);
    if (!decoded) return false;

    // Supply a real script index from the installed map. Only this cached
    // prototype's SID changes, and it is restored immediately after application.
    Script* scriptFixture = nullptr;
    int scriptType = -1;
    for (const auto& door : worldDoors) {
        if (door.second->sid != -1 && scr_ptr(door.second->sid, &scriptFixture) == 0) {
            scriptType = SID_TYPE(door.second->sid);
            break;
        }
    }
    Proto* stagedPrototype = nullptr;
    if (scriptFixture == nullptr || proto_ptr(scenery.pid, &stagedPrototype) != 0) return false;
    int originalSid = stagedPrototype->sid;
    stagedPrototype->sid = (scriptType << 24) | scriptFixture->scr_script_idx;

    struct RegistryMapping {
        EntityId id;
        Object* object;
        std::optional<PlayerId> owner;
    };
    std::vector<RegistryMapping> mappings;
    auto recordMappings = [&mappings](const auto& entries) {
        for (const auto& entry : entries) {
            mappings.push_back({ entry.first, entry.second, session.entities().ownerOf(entry.first) });
        }
    };
    recordMappings(worldExitGrids);
    recordMappings(worldDoors);
    recordMappings(worldScenery);
    recordMappings(worldCritters);
    recordMappings(worldItems);
    for (const auto& actor : baseline.actors) {
        mappings.push_back({ actor.entityId, session.entities().findObject(actor.entityId),
            session.entities().ownerOf(actor.entityId) });
    }
    int bodiesBefore = nativeSnapshotBodyCount();
    int scriptsBefore = nativeSnapshotScriptCount();
    std::size_t registryBefore = session.entities().size();
    std::vector<QueueEventState> queueBefore;
    if (!queue_capture_state(queueBefore)) { stagedPrototype->sid = originalSid; return false; }
    int attemptsBefore = snapshotPlacementAttempts;
    engineExecutionProbeBegin();
    nativeSnapshotArmObjectFailure(3);
    bool rejected = !networkWorldApplySnapshot(decoded.snapshot);
    NativeSnapshotAllocationFailure fault = nativeSnapshotDisarmObjectFailure();
    stagedPrototype->sid = originalSid;
    WorldSnapshot after;
    bool captured = networkWorldCaptureSnapshot({}, after);
    auto afterDigest = computeSnapshotDigest(after);
    EngineExecutionProbeCounts effects = engineExecutionProbeEnd();
    std::vector<QueueEventState> queueAfter;
    bool queueUnchanged = queue_capture_state(queueAfter) && sameQueue(queueBefore, queueAfter);
    bool registryUnchanged = registryBefore == session.entities().size();
    for (const auto& mapping : mappings) {
        registryUnchanged = registryUnchanged
            && session.entities().findObject(mapping.id) == mapping.object
            && session.entities().findEntity(mapping.object) == mapping.id
            && session.entities().ownerOf(mapping.id) == mapping.owner;
    }
    bool preserved = rejected && captured && afterDigest
        && afterDigest.digest.overall == baselineDigest.digest.overall && registryUnchanged
        && bodiesBefore == nativeSnapshotBodyCount() && scriptsBefore == nativeSnapshotScriptCount()
        && fault.requests == 4 && fault.failures == 1 && fault.bodiesAtFailure == bodiesBefore + 3
        && fault.scriptsAtFailure == scriptsBefore + 1 && queueUnchanged
        && snapshotPlacementAttempts == attemptsBefore
        && effects.scriptProcedures == 0 && effects.combatAttacks == 0 && effects.randomDraws == 0;
    std::fprintf(stderr,
        "NATIVE_SNAPSHOT_CREATION_FAILURE_CONTROL fourth_object=1 nested_item=1 requests=%d faults=%d rejected=%d preserved=%d captured=%d registry=%zu/%zu mappings=%d bodies=%d/%d bodies_at_fault=%d scripts=%d/%d scripts_at_fault=%d queue=%d attempts=%d scripts_run=%u attacks=%u rng=%u\n",
        fault.requests, fault.failures, rejected, preserved, captured,
        registryBefore, session.entities().size(), registryUnchanged, bodiesBefore, nativeSnapshotBodyCount(),
        fault.bodiesAtFailure, scriptsBefore, nativeSnapshotScriptCount(), fault.scriptsAtFailure, queueUnchanged,
        snapshotPlacementAttempts - attemptsBefore, effects.scriptProcedures, effects.combatAttacks, effects.randomDraws);
    if (!preserved) return false;

    // The authoritative NPC hex is occupied by a player. Restore that exact
    // coordinate and the nested inventory, without native best-effort placement.
    attemptsBefore = snapshotPlacementAttempts;
    engineExecutionProbeBegin();
    bool applied = networkWorldApplySnapshot(decoded.snapshot);
    WorldSnapshot appliedSnapshot;
    bool appliedCaptured = networkWorldCaptureSnapshot({}, appliedSnapshot);
    auto expectedDigest = computeSnapshotDigest(decoded.snapshot);
    auto appliedDigest = computeSnapshotDigest(appliedSnapshot);
    Object* placedNpc = session.entities().findObject(npc.entityId);
    Object* placedChild = session.entities().findObject(child.entityId);
    bool exactPlacement = placedNpc != nullptr && placedNpc->tile == npc.tile && placedNpc->elevation == npc.elevation;
    bool nestedHolder = placedChild != nullptr && placedChild->owner == session.entities().findObject(bag.entityId);
    bool restored = networkWorldApplySnapshot(baseline);
    WorldSnapshot restoredSnapshot;
    bool restoredCaptured = networkWorldCaptureSnapshot({}, restoredSnapshot);
    auto restoredDigest = computeSnapshotDigest(restoredSnapshot);
    EngineExecutionProbeCounts positiveEffects = engineExecutionProbeEnd();
    bool matched = applied && appliedCaptured && expectedDigest && appliedDigest
        && expectedDigest.digest.overall == appliedDigest.digest.overall && exactPlacement && nestedHolder;
    bool baselineRestored = restored && restoredCaptured && restoredDigest
        && restoredDigest.digest.overall == baselineDigest.digest.overall
        && bodiesBefore == nativeSnapshotBodyCount() && scriptsBefore == nativeSnapshotScriptCount()
        && registryBefore == session.entities().size();
    bool placementPassed = matched && baselineRestored && snapshotPlacementAttempts == attemptsBefore
        && positiveEffects.scriptProcedures == 0 && positiveEffects.combatAttacks == 0 && positiveEffects.randomDraws == 0;
    std::fprintf(stderr,
        "NATIVE_SNAPSHOT_CREATION_PLACEMENT_CONTROL blocked_authoritative_hex=1 applied=%d matched=%d exact=%d nested_holder=%d restored=%d attempts=%d scripts_run=%u attacks=%u rng=%u\n",
        applied, matched, exactPlacement, nestedHolder, baselineRestored, snapshotPlacementAttempts - attemptsBefore,
        positiveEffects.scriptProcedures, positiveEffects.combatAttacks, positiveEffects.randomDraws);
    return placementPassed;
}
} // namespace

bool networkWorldRunEngineAuthoritySmokeTest(EngineExecutionProbeCounts& counts)
{
    if (!nativeSnapshotOriginalAuthoritySmokeTest(counts)) return false;
    return worldMode != NetworkLaunchMode::Join || runNativeSnapshotCreationControl();
}

} // namespace multiplayer
} // namespace fallout
