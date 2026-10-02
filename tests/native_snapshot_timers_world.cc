#include "game/protinst.h"
#include <cstdlib>
#include <cstring>
#include "native_snapshot_timers_support.h"

namespace fallout {
namespace {
int snapshotPlacementAttempts = 0;
int nativeSnapshotTimersAttemptPlacement(Object* object, int tile, int elevation, int radius)
{
    ++snapshotPlacementAttempts;
    return obj_attempt_placement(object, tile, elevation, radius);
}
} // namespace
} // namespace fallout

// Compile the actual world bridge with a private wrapper. Runtime and the
// desktop entry point remain the normal game objects in the linked fixture.
#define obj_attempt_placement nativeSnapshotTimersAttemptPlacement
#define networkWorldRunEngineAuthoritySmokeTest nativeSnapshotTimersOriginalAuthoritySmokeTest
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

struct RegistryMapping {
    EntityId id;
    Object* object;
    std::optional<PlayerId> owner;
};

std::vector<RegistryMapping> captureRegistryMappings(const WorldSnapshot& snapshot)
{
    std::vector<RegistryMapping> mappings;
    auto record = [&mappings](const auto& entries) {
        for (const auto& entry : entries) {
            mappings.push_back({ entry.first, entry.second, session.entities().ownerOf(entry.first) });
        }
    };
    record(worldExitGrids);
    record(worldDoors);
    record(worldScenery);
    record(worldCritters);
    record(worldItems);
    for (const auto& actor : snapshot.actors) {
        mappings.push_back({ actor.entityId, session.entities().findObject(actor.entityId),
            session.entities().ownerOf(actor.entityId) });
    }
    return mappings;
}

bool registryMappingsUnchanged(const std::vector<RegistryMapping>& mappings, std::size_t size)
{
    if (session.entities().size() != size) return false;
    for (const auto& mapping : mappings) {
        if (session.entities().findObject(mapping.id) != mapping.object
            || session.entities().findEntity(mapping.object) != mapping.id
            || session.entities().ownerOf(mapping.id) != mapping.owner) return false;
    }
    return true;
}

bool runItemTimerOwnerControls()
{
    WorldSnapshot baseline;
    std::vector<QueueEventState> nativeQueue;
    if (!networkWorldCaptureSnapshot({}, baseline) || baseline.actors.empty()
        || !queue_capture_state(nativeQueue)) return false;
    auto baselineDigest = computeSnapshotDigest(baseline);
    auto chosen = std::find_if(baseline.items.begin(), baseline.items.end(), [](const auto& item) {
        Proto* prototype = nullptr;
        return isValid(item.holderId) && (item.objectFlags & (OBJECT_USED | OBJECT_EQUIPPED)) == 0
            && proto_ptr(item.itemDescriptor.pid, &prototype) == 0
            && prototype->item.type != ITEM_TYPE_CONTAINER;
    });
    if (!baselineDigest || chosen == baseline.items.end()) return false;
    int bodiesBefore = nativeSnapshotTimersBodyCount();
    int scriptsBefore = nativeSnapshotTimersScriptCount();
    std::size_t registryBefore = session.entities().size();
    auto mappings = captureRegistryMappings(baseline);
    TimedEventSnapshot event;
    event.time = std::max(1, baseline.gameTime);
    for (const auto& timer : baseline.timedEvents) event.time = std::max(event.time, timer.time);
    ++event.time;
    event.eventType = EVENT_TYPE_FLARE;
    event.ownerId = chosen->entityId;
    WorldSnapshot positive = baseline;
    auto owner = std::find_if(positive.items.begin(), positive.items.end(), [&](const auto& item) {
        return item.entityId == chosen->entityId;
    });
    owner->objectFlags |= OBJECT_USED;
    positive.timedEvents.push_back(event);
    std::vector<std::uint8_t> packet;
    if (encodeSnapshot(positive, packet) != SnapshotError::None) return false;
    auto positiveDecoded = decodeSnapshot(packet);
    if (!positiveDecoded) return false;
    engineExecutionProbeBegin();
    bool applied = networkWorldApplySnapshot(positiveDecoded.snapshot);
    WorldSnapshot afterPositive;
    bool positiveCaptured = networkWorldCaptureSnapshot({}, afterPositive);
    auto expectedDigest = computeSnapshotDigest(positiveDecoded.snapshot);
    auto positiveDigest = computeSnapshotDigest(afterPositive);
    std::vector<QueueEventState> queued;
    bool nativeOwner = queue_capture_state(queued)
        && std::any_of(queued.begin(), queued.end(), [&](const auto& timer) {
            return timer.eventType == event.eventType && timer.time == event.time
                && timer.owner == session.entities().findObject(chosen->entityId)
                && timer.owner != nullptr && (timer.owner->flags & OBJECT_USED) != 0;
        });
    bool matched = applied && positiveCaptured && expectedDigest && positiveDigest && nativeOwner
        && expectedDigest.digest.overall == positiveDigest.digest.overall;
    bool restored = networkWorldApplySnapshot(baseline);
    WorldSnapshot restoredSnapshot;
    bool restoredCaptured = networkWorldCaptureSnapshot({}, restoredSnapshot);
    auto restoredDigest = computeSnapshotDigest(restoredSnapshot);
    std::vector<QueueEventState> restoredQueue;
    auto positiveEffects = engineExecutionProbeEnd();
    bool positiveMappings = registryMappingsUnchanged(mappings, registryBefore);
    bool baselineRestored = restored && restoredCaptured && restoredDigest
        && restoredDigest.digest.overall == baselineDigest.digest.overall
        && queue_capture_state(restoredQueue) && sameQueue(nativeQueue, restoredQueue)
        && bodiesBefore == nativeSnapshotTimersBodyCount() && scriptsBefore == nativeSnapshotTimersScriptCount()
        && positiveMappings && positiveEffects.scriptProcedures == 0
        && positiveEffects.combatAttacks == 0 && positiveEffects.randomDraws == 0;
    std::fprintf(stderr,
        "NATIVE_SNAPSHOT_TIMERS_ITEM_OWNER_POSITIVE queued_used=1 checksummed=1 applied=%d matched=%d restored=%d registry=%zu/%zu mappings=%d bodies=%d/%d scripts=%d/%d queue=%d scripts_run=%u attacks=%u rng=%u\n",
        applied, matched, baselineRestored, registryBefore, session.entities().size(), positiveMappings,
        bodiesBefore, nativeSnapshotTimersBodyCount(), scriptsBefore, nativeSnapshotTimersScriptCount(),
        sameQueue(nativeQueue, restoredQueue), positiveEffects.scriptProcedures,
        positiveEffects.combatAttacks, positiveEffects.randomDraws);
    if (!matched || !baselineRestored) return false;
    // Old allocator controls isolate allocation failure from this later guard.
    if (std::getenv("NATIVE_SNAPSHOT_TIMER_ALLOCATION_NEGATIVE") != nullptr) return true;

    WorldSnapshot negative = baseline;
    std::uint32_t nextId = 1;
    auto include = [&](const auto& entries) {
        for (const auto& entry : entries) nextId = std::max(nextId, entry.entityId.value + 1);
    };
    include(negative.actors);
    include(negative.doors);
    include(negative.scenery);
    include(negative.critters);
    include(negative.items);
    while (session.entities().contains(EntityId { nextId })) ++nextId;
    ItemSnapshot duplicate = *chosen;
    duplicate.entityId = EntityId { nextId };
    duplicate.quantity = 1;
    negative.items.push_back(duplicate);
    negative.timedEvents.push_back(event);
    ++negative.actors.front().hitPoints;
    negative.mapLocalVariables.push_back(98765);
    if (encodeSnapshot(negative, packet) != SnapshotError::None) return false;
    auto negativeDecoded = decodeSnapshot(packet);
    if (!negativeDecoded) return false;
    engineExecutionProbeBegin();
    bool rejected = !networkWorldApplySnapshot(negativeDecoded.snapshot);
    WorldSnapshot afterNegative;
    bool captured = networkWorldCaptureSnapshot({}, afterNegative);
    auto afterDigest = computeSnapshotDigest(afterNegative);
    std::vector<QueueEventState> afterQueue;
    bool queueUnchanged = queue_capture_state(afterQueue) && sameQueue(nativeQueue, afterQueue);
    auto effects = engineExecutionProbeEnd();
    bool mappingUnchanged = registryMappingsUnchanged(mappings, registryBefore);
    bool preserved = rejected && captured && afterDigest
        && afterDigest.digest.overall == baselineDigest.digest.overall && mappingUnchanged
        && bodiesBefore == nativeSnapshotTimersBodyCount() && scriptsBefore == nativeSnapshotTimersScriptCount()
        && queueUnchanged && effects.scriptProcedures == 0 && effects.combatAttacks == 0 && effects.randomDraws == 0;
    std::fprintf(stderr,
        "NATIVE_SNAPSHOT_TIMERS_ITEM_OWNER_REJECTION missing_used=1 matching_duplicate=1 checksummed=1 rejected=%d preserved=%d captured=%d registry=%zu/%zu mappings=%d bodies=%d/%d scripts=%d/%d queue=%d old_owner_present=%d duplicate_present=%d scripts_run=%u attacks=%u rng=%u\n",
        rejected, preserved, captured, registryBefore, session.entities().size(), mappingUnchanged,
        bodiesBefore, nativeSnapshotTimersBodyCount(), scriptsBefore, nativeSnapshotTimersScriptCount(),
        queueUnchanged, session.entities().findObject(chosen->entityId) != nullptr,
        session.entities().findObject(duplicate.entityId) != nullptr,
        effects.scriptProcedures, effects.combatAttacks, effects.randomDraws);
    return preserved;
}

#ifdef NATIVE_SNAPSHOT_TIMERS_PREPARED_OWNER_CONTROL
bool runPreparedQueueOwnerControl()
{
    WorldSnapshot baseline;
    std::vector<QueueEventState> original;
    if (!networkWorldCaptureSnapshot({}, baseline) || baseline.actors.size() < 2
        || !queue_capture_state(original)) return false;
    Object* first = session.entities().findObject(baseline.actors[0].entityId);
    Object* final = session.entities().findObject(baseline.actors[1].entityId);
    if (first == nullptr || final == nullptr || first == final) return false;
    int firstFlags = first->flags;
    int finalFlags = final->flags;
    first->flags &= ~OBJECT_USED;
    final->flags &= ~OBJECT_USED;
    QueueEventState event;
    event.time = std::max(1, baseline.gameTime);
    event.eventType = EVENT_TYPE_RADIATION;
    event.owner = first;
    event.payloadCount = 2;
    event.payload[0] = 1;
    PreparedQueueEvents prepared;
    bool staged = prepared.prepare({ event });
    bool flagsUntouched = (first->flags & OBJECT_USED) == 0 && (final->flags & OBJECT_USED) == 0;
    std::vector<Object*> owners { final };
    bool rebound = staged && prepared.rebindOwners(owners);
    engineExecutionProbeBegin();
    nativeSnapshotTimersArmQueueFailure(0);
    bool committed = rebound && prepared.commit();
    auto fault = nativeSnapshotTimersDisarmQueueFailure();
    auto effects = engineExecutionProbeEnd();
    std::vector<QueueEventState> after;
    bool exact = queue_capture_state(after) && after.size() == 1 && after[0].owner == final
        && (first->flags & OBJECT_USED) == 0 && (final->flags & OBJECT_USED) != 0;
    bool restored = queue_replace_state(original);
    first->flags = firstFlags;
    final->flags = finalFlags;
    bool passed = staged && flagsUntouched && rebound && committed && exact && restored
        && fault.requests == 0 && fault.failures == 0 && fault.liveBlocks == 0
        && effects.scriptProcedures == 0 && effects.combatAttacks == 0 && effects.randomDraws == 0;
    std::fprintf(stderr, "NATIVE_SNAPSHOT_TIMERS_OWNER_REBIND_CONTROL staged=%d flags_untouched=%d rebound=%d committed=%d exact=%d restored=%d requests=%d faults=%d passed=%d\n",
        staged, flagsUntouched, rebound, committed, exact, restored, fault.requests, fault.failures, passed);
    return passed;
}
#endif

bool runNativeSnapshotTimersControl()
{
#ifdef NATIVE_SNAPSHOT_TIMERS_PREPARED_OWNER_CONTROL
    if (!runPreparedQueueOwnerControl()) return false;
#endif
    if (!runItemTimerOwnerControls()) return false;
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
    // Two payload-bearing native events force a failure after an earlier complete event.
    int lastTime = std::max(1, control.gameTime);
    int baselineQueueRequests = 0;
    for (const auto& event : control.timedEvents) {
        lastTime = std::max(lastTime, event.time);
        baselineQueueRequests += 1 + (event.payloadCount != 0);
    }
    TimedEventSnapshot radiation;
    radiation.time = lastTime + 1;
    radiation.eventType = EVENT_TYPE_RADIATION;
    radiation.ownerId = npc.entityId;
    radiation.payloadCount = 2;
    radiation.payload[0] = 1;
    radiation.payload[1] = 0;
    control.timedEvents.push_back(radiation);
    radiation.time += 1;
    radiation.ownerId = control.actors.front().entityId;
    radiation.payload[1] = 1;
    control.timedEvents.push_back(radiation);
    control.actors.front().objectFlags |= OBJECT_USED;
    control.critters.back().objectFlags |= OBJECT_USED;
    const char* faultMode = std::getenv("NATIVE_SNAPSHOT_TIMER_FAULT");
    bool nodeFault = faultMode != nullptr && std::strcmp(faultMode, "node") == 0;
    int faultRequest = baselineQueueRequests + (nodeFault ? 4 : 3);
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

    auto mappings = captureRegistryMappings(baseline);
    int bodiesBefore = nativeSnapshotTimersBodyCount();
    int scriptsBefore = nativeSnapshotTimersScriptCount();
    std::size_t registryBefore = session.entities().size();
    std::vector<QueueEventState> queueBefore;
    if (!queue_capture_state(queueBefore)) { stagedPrototype->sid = originalSid; return false; }
    int attemptsBefore = snapshotPlacementAttempts;
    engineExecutionProbeBegin();
    nativeSnapshotTimersArmQueueFailure(faultRequest - 1);
    bool rejected = !networkWorldApplySnapshot(decoded.snapshot);
    NativeSnapshotTimersAllocationFailure fault = nativeSnapshotTimersDisarmQueueFailure();
    stagedPrototype->sid = originalSid;
    WorldSnapshot after;
    bool captured = networkWorldCaptureSnapshot({}, after);
    auto afterDigest = computeSnapshotDigest(after);
    EngineExecutionProbeCounts effects = engineExecutionProbeEnd();
    std::vector<QueueEventState> queueAfter;
    bool queueUnchanged = queue_capture_state(queueAfter) && sameQueue(queueBefore, queueAfter);
    bool registryUnchanged = registryMappingsUnchanged(mappings, registryBefore);
    bool preserved = rejected && captured && afterDigest
        && afterDigest.digest.overall == baselineDigest.digest.overall && registryUnchanged
        && bodiesBefore == nativeSnapshotTimersBodyCount() && scriptsBefore == nativeSnapshotTimersScriptCount()
        && fault.requests == faultRequest && fault.failures == 1 && fault.bodiesAtFailure == bodiesBefore + 4
        && fault.scriptsAtFailure == scriptsBefore + 1 && queueUnchanged
        && fault.liveBlocks == 0 && snapshotPlacementAttempts == attemptsBefore
        && effects.scriptProcedures == 0 && effects.combatAttacks == 0 && effects.randomDraws == 0;
    std::fprintf(stderr,
        "NATIVE_SNAPSHOT_TIMERS_FAILURE_CONTROL node=%d nested_item=1 expected_requests=%d requests=%d faults=%d rejected=%d preserved=%d captured=%d registry=%zu/%zu mappings=%d bodies=%d/%d bodies_at_fault=%d scripts=%d/%d scripts_at_fault=%d queue=%d live_blocks=%d attempts=%d scripts_run=%u attacks=%u rng=%u\n",
        nodeFault, faultRequest, fault.requests, fault.failures, rejected, preserved, captured,
        registryBefore, session.entities().size(), registryUnchanged, bodiesBefore, nativeSnapshotTimersBodyCount(),
        fault.bodiesAtFailure, scriptsBefore, nativeSnapshotTimersScriptCount(), fault.scriptsAtFailure, queueUnchanged, fault.liveBlocks,
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
        && bodiesBefore == nativeSnapshotTimersBodyCount() && scriptsBefore == nativeSnapshotTimersScriptCount()
        && registryBefore == session.entities().size();
    bool placementPassed = matched && baselineRestored && snapshotPlacementAttempts == attemptsBefore
        && positiveEffects.scriptProcedures == 0 && positiveEffects.combatAttacks == 0 && positiveEffects.randomDraws == 0;
    std::fprintf(stderr,
        "NATIVE_SNAPSHOT_TIMERS_PLACEMENT_CONTROL blocked_authoritative_hex=1 applied=%d matched=%d exact=%d nested_holder=%d restored=%d attempts=%d scripts_run=%u attacks=%u rng=%u\n",
        applied, matched, exactPlacement, nestedHolder, baselineRestored, snapshotPlacementAttempts - attemptsBefore,
        positiveEffects.scriptProcedures, positiveEffects.combatAttacks, positiveEffects.randomDraws);
    return placementPassed;
}
} // namespace

bool networkWorldRunEngineAuthoritySmokeTest(EngineExecutionProbeCounts& counts)
{
    if (!nativeSnapshotTimersOriginalAuthoritySmokeTest(counts)) return false;
    return worldMode != NetworkLaunchMode::Join || runNativeSnapshotTimersControl();
}

} // namespace multiplayer
} // namespace fallout
