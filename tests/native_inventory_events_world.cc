#include "game/protinst.h"
#include "multiplayer/gameplay_wire.h"
#include "native_snapshot_creation_support.h"
#define networkWorldRunEngineAuthoritySmokeTest nativeTransferOriginalAuthoritySmokeTest
#include "multiplayer/network_world.cc"
#undef networkWorldRunEngineAuthoritySmokeTest
#include <cstdlib>

namespace fallout {
namespace multiplayer {
namespace {

std::uint64_t nativeInventoryDigest()
{
    std::uint64_t hash = 1469598103934665603ULL;
    auto add = [&](std::uint64_t value) { hash ^= value; hash *= 1099511628211ULL; };
    std::unordered_set<Object*> seen;
    auto visit = [&](auto&& visit, Object* object) -> void {
        if (object == nullptr || !seen.insert(object).second) return;
        add(object->id); add(object->pid); add(object->owner != nullptr ? object->owner->id : 0);
        add(object->flags); add(object->data.inventory.length);
        for (int i = 0; i < object->data.inventory.length; ++i) {
            add(object->data.inventory.items[i].quantity);
            visit(visit, object->data.inventory.items[i].item);
        }
    };
    for (PlayerId id : session.players().playerIds()) visit(visit, networkWorldPlayerActor(id));
    return hash;
}

int nativePidQuantity(Object* root, int pid)
{
    int quantity = 0;
    std::unordered_set<Object*> seen;
    auto visit = [&](auto&& visit, Object* object) -> void {
        if (object == nullptr || !seen.insert(object).second) return;
        for (int i = 0; i < object->data.inventory.length; ++i) {
            const InventoryItem& entry = object->data.inventory.items[i];
            if (entry.item->pid == pid) quantity += entry.quantity;
            visit(visit, entry.item);
        }
    };
    visit(visit, root);
    return quantity;
}

int nativeBodyCount()
{
    return nativeSnapshotBodyCount();
}

bool roundTrip(const InventoryTransferredEvent& transfer, InventoryTransferredEvent& result)
{
    ProtocolEnvelope envelope;
    envelope.sessionId = SessionId { 1 };
    envelope.sequence = 1;
    GameEvent event { EventSequence { 1 }, CommandSequence { 1 }, transfer };
    if (encodeGameEvent(event, envelope) != GameplayWireError::None) return false;
    auto decoded = decodeGameEvent(envelope);
    if (!decoded) return false;
    auto* payload = std::get_if<InventoryTransferredEvent>(&decoded.event.payload);
    if (payload == nullptr) return false;
    result = *payload;
    return true;
}

bool runMissingPositive(bool drop)
{
    WorldSnapshot before;
    if (!networkWorldCaptureSnapshot({}, before) || before.actors.size() != 2) return false;
    EntityId sourceId = before.actors.front().entityId;
    EntityId destinationId = before.actors.back().entityId;
    Object* source = session.entities().findObject(sourceId);
    Object* destination = session.entities().findObject(destinationId);
    std::uint32_t next = 1;
    while (session.entities().contains(EntityId { next })) ++next;
    EntityId itemId { next++ };
    while (session.entities().contains(EntityId { next })) ++next;
    EntityId remainderId { next };
    ItemDescriptor descriptor;
    descriptor.pid = 211;
    const auto registryBefore = session.entities().size();
    const int bodiesBefore = nativeSnapshotBodyCount();
    bool codec = false;
    bool applied = false;
    bool replay = false;
    InventoryTransferredEvent transfer { sourceId, sourceId, destinationId, itemId, 1, 2, remainderId, descriptor };
    ItemDroppedEvent itemDrop { sourceId, sourceId, itemId, 1, 1, {}, source->tile, source->elevation, descriptor };
    if (drop) {
        ProtocolEnvelope envelope;
        envelope.sessionId = SessionId { 1 };
        envelope.sequence = 1;
        GameEvent event { EventSequence { 1 }, CommandSequence { 1 }, itemDrop };
        codec = encodeGameEvent(event, envelope) == GameplayWireError::None;
        if (!codec) return false;
        auto decoded = decodeGameEvent(envelope);
        auto* payload = decoded ? std::get_if<ItemDroppedEvent>(&decoded.event.payload) : nullptr;
        if (payload == nullptr) return false;
        itemDrop = *payload;
        applied = networkWorldApplyItemDrop(itemDrop);
    } else {
        InventoryTransferredEvent decoded;
        codec = roundTrip(transfer, decoded);
        if (!codec) return false;
        transfer = decoded;
        applied = networkWorldApplyInventoryTransfer(transfer);
    }
    Object* item = session.entities().findObject(itemId);
    Object* remainder = drop ? nullptr : session.entities().findObject(remainderId);
    bool exact = applied && item != nullptr
        && (drop ? item->owner == nullptr && item->tile == source->tile && item->elevation == source->elevation
                 : item->owner == destination && item_count(destination, item) == 1
                     && remainder != nullptr && remainder->owner == source && item_count(source, remainder) == 1);
    WorldSnapshot after;
    auto captured = networkWorldCaptureSnapshot({}, after);
    auto digest = captured ? computeSnapshotDigest(after) : SnapshotDigestResult {};
    if (exact) replay = drop ? networkWorldApplyItemDrop(itemDrop) : networkWorldApplyInventoryTransfer(transfer);
    WorldSnapshot replayed;
    auto replayCaptured = networkWorldCaptureSnapshot({}, replayed);
    auto replayDigest = replayCaptured ? computeSnapshotDigest(replayed) : SnapshotDigestResult {};
    bool replayPreserved = replay && digest && replayDigest && digest.digest.overall == replayDigest.digest.overall;
    const int bodiesAfter = nativeSnapshotBodyCount();
    const auto registryAfter = session.entities().size();
    bool counts = bodiesAfter == bodiesBefore + (drop ? 1 : 2)
        && registryAfter == registryBefore + (drop ? 1 : 2);
    // Restore the fixture setup before the normal smoke continuation. Clean
    // process exits additionally exercise native inventory/map lifetime cleanup.
    bool restored = exact && replayPreserved && counts && networkWorldApplySnapshot(before);
    std::fprintf(stderr, "NATIVE_INVENTORY_EVENT_POSITIVE mode=%s codec=%d applied=%d exact=%d replay=%d counts=%d restored=%d registry=%zu/%zu bodies=%d/%d\n",
        drop ? "missing-drop" : "missing-transfer", codec, applied, exact, replayPreserved, counts, restored,
        registryBefore, registryAfter, bodiesBefore, bodiesAfter);
    return codec && exact && replayPreserved && counts && restored;
}

bool runTransferBoundary()
{
    const char* setting = std::getenv("FALLOUT_INVENTORY_EVENT_BOUNDARY");
    bool cycle = setting != nullptr && std::string(setting) == "cycle";
    bool missing = setting != nullptr && std::string(setting) == "missing";
    WorldSnapshot initial;
    if (!networkWorldCaptureSnapshot({}, initial) || initial.actors.size() != 2 || initial.doors.empty()) return (std::fprintf(stderr, "NATIVE_TRANSFER_SETUP_STOP line=%d\n", __LINE__), false);
    EntityId sourceId = initial.actors.front().entityId;
    EntityId destinationId = initial.actors.back().entityId;
    Object* source = session.entities().findObject(sourceId);
    Object* destination = session.entities().findObject(destinationId);
    Object* item = nullptr;
    Object* inner = nullptr;
    EntityId itemId {};
    std::uint32_t available = 0;
    if (cycle) {
        // Explicit native fixture setup, never a claimed gameplay acquisition.
        if (obj_pid_new(&item, 211) != 0 || item_add_force(source, item, 1) != 0
            || obj_disconnect(item, nullptr) != 0) return (std::fprintf(stderr, "NATIVE_TRANSFER_SETUP_STOP line=%d\n", __LINE__), false);
        auto outerId = networkWorldEnsureItemRegistered(item);
        if (!outerId) return (std::fprintf(stderr, "NATIVE_TRANSFER_SETUP_STOP line=%d\n", __LINE__), false);
        itemId = *outerId;
        if (obj_pid_new(&inner, 211) != 0 || item_add_force(item, inner, 1) != 0
            || obj_disconnect(inner, nullptr) != 0) return (std::fprintf(stderr, "NATIVE_TRANSFER_SETUP_STOP line=%d\n", __LINE__), false);
        auto innerId = networkWorldEnsureItemRegistered(inner);
        if (!innerId) return (std::fprintf(stderr, "NATIVE_TRANSFER_SETUP_STOP line=%d\n", __LINE__), false);
        destination = inner;
        destinationId = *innerId;
        available = 1;
    } else if (missing) {
        std::uint32_t unusedId = 1;
        for (const auto& state : initial.items) unusedId = std::max(unusedId, state.entityId.value + 1);
        for (const auto& state : initial.critters) unusedId = std::max(unusedId, state.entityId.value + 1);
        while (session.entities().contains(EntityId { unusedId })) ++unusedId;
        itemId = EntityId { unusedId };
        available = 2;
    } else {
        for (const ItemSnapshot& state : initial.items) {
            Object* candidate = session.entities().findObject(state.entityId);
            if (state.holderId == sourceId && state.quantity >= 2 && candidate != nullptr
                && (candidate->flags & (OBJECT_EQUIPPED | OBJECT_USED)) == 0
                && item_get_type(candidate) != ITEM_TYPE_CONTAINER) {
                item = candidate; itemId = state.entityId; available = state.quantity; break;
            }
        }
        if (item == nullptr) return (std::fprintf(stderr, "NATIVE_TRANSFER_SETUP_STOP line=%d\n", __LINE__), false);
    }
    WorldSnapshot before;
    if (!networkWorldCaptureSnapshot({}, before)) return (std::fprintf(stderr, "NATIVE_TRANSFER_SETUP_STOP line=%d\n", __LINE__), false);
    auto beforeDigest = computeSnapshotDigest(before);
    if (!beforeDigest) return (std::fprintf(stderr, "NATIVE_TRANSFER_SETUP_STOP line=%d\n", __LINE__), false);
    ItemDescriptor descriptor;
    if (missing) descriptor.pid = 211;
    if (!missing && !describeItem(item, descriptor)) return (std::fprintf(stderr, "NATIVE_TRANSFER_SETUP_STOP line=%d\n", __LINE__), false);
    InventoryTransferredEvent injected { sourceId, sourceId, destinationId, itemId,
        1, available, cycle ? EntityId {} : before.doors.front().entityId, descriptor };
    InventoryTransferredEvent decoded;
    bool codecAccepted = roundTrip(injected, decoded);
    if (!codecAccepted) return (std::fprintf(stderr, "NATIVE_TRANSFER_SETUP_STOP line=%d\n", __LINE__), false);
    Object* collisionOccupant = cycle ? nullptr : session.entities().findObject(injected.remainderItemId);
    std::size_t registryBefore = session.entities().size();
    int bodiesBefore = nativeBodyCount();
    int scriptsBefore = nativeSnapshotScriptCount();
    int sourceBefore = nativePidQuantity(source, descriptor.pid);
    int destinationBefore = nativePidQuantity(destination, descriptor.pid);
    Object* ownerBefore = item != nullptr ? item->owner : nullptr;
    std::uint64_t inventoryBefore = nativeInventoryDigest();
    engineExecutionProbeBegin();
    bool applied = networkWorldApplyInventoryTransfer(decoded);
    auto effects = engineExecutionProbeEnd();
    if (missing) item = session.entities().findObject(itemId);
    bool ownershipChanged = item != nullptr && item->owner != ownerBefore;
    bool cycleObserved = cycle && item->owner == inner && inner->owner == item
        && inner->data.inventory.length == 1 && inner->data.inventory.items[0].item == item;
    int sourceAfter = nativePidQuantity(source, descriptor.pid);
    int destinationAfter = nativePidQuantity(destination, descriptor.pid);
    std::size_t registryAfter = session.entities().size();
    int bodiesAfter = nativeBodyCount();
    int scriptsAfter = nativeSnapshotScriptCount();
    std::uint64_t inventoryAfter = nativeInventoryDigest();
    bool collisionIntact = cycle || session.entities().findObject(injected.remainderItemId) == collisionOccupant;
    // Snapshot capture follows the ownership graph recursively. Repair the
    // deliberately injected cycle before capture or lifetime cleanup.
    bool repaired = !cycleObserved;
    if (cycleObserved) {
        repaired = item_remove_mult(inner, item, 1) == 0 && item_add_force(source, item, 1) == 0;
    }
    WorldSnapshot after;
    bool captured = repaired && networkWorldCaptureSnapshot({}, after);
    auto afterDigest = captured ? computeSnapshotDigest(after) : SnapshotDigestResult {};
    bool restoredDigest = cycle && afterDigest && afterDigest.digest.overall == beforeDigest.digest.overall;
    std::fprintf(stderr,
        "NATIVE_TRANSFER_BOUNDARY_CONTROL mode=%s fixture_injected=1 codec_accepted=%d applied=%d ownership_changed=%d cycle=%d repaired=%d snapshot_captured=%d digest_before=%llu digest_after=%llu inventory_before=%llu inventory_after=%llu post_repair_digest_matches=%d registry=%zu/%zu bodies=%d/%d scripts=%d/%d source_quantity=%d/%d destination_quantity=%d/%d collision_intact=%d native_scripts=%u attacks=%u rng=%u\n",
        cycle ? "cycle" : missing ? "missing" : "remainder", codecAccepted, applied, ownershipChanged, cycleObserved, repaired,
        captured, (unsigned long long)beforeDigest.digest.overall,
        (unsigned long long)(afterDigest ? afterDigest.digest.overall : 0),
        (unsigned long long)inventoryBefore, (unsigned long long)inventoryAfter, restoredDigest,
        registryBefore, registryAfter, bodiesBefore, bodiesAfter, scriptsBefore, scriptsAfter,
        sourceBefore, sourceAfter, destinationBefore, destinationAfter, collisionIntact,
        effects.scriptProcedures, effects.combatAttacks, effects.randomDraws);
    bool preserved = !applied && captured && afterDigest
        && afterDigest.digest.overall == beforeDigest.digest.overall
        && inventoryBefore == inventoryAfter && registryBefore == registryAfter
        && bodiesBefore == bodiesAfter && scriptsBefore == scriptsAfter
        && collisionIntact && !ownershipChanged && !cycleObserved
        && effects.scriptProcedures == 0 && effects.combatAttacks == 0 && effects.randomDraws == 0;
    if (std::getenv("FALLOUT_INVENTORY_EVENT_EXPECT_NEGATIVE") == nullptr) {
        std::fprintf(stderr, "NATIVE_INVENTORY_EVENT_REJECTION mode=%s preserved=%d\n", setting, preserved);
        return preserved;
    }
    bool demonstrated = cycle
        ? applied && cycleObserved && ownershipChanged && repaired && restoredDigest
        : !applied && ownershipChanged && item->owner == destination && inventoryBefore != inventoryAfter && collisionIntact;
    std::fprintf(stderr, "NATIVE_TRANSFER_BOUNDARY_NEGATIVE_%s mode=%s intentional_stop_before_normal_continuation=1\n",
        demonstrated ? "CONFIRMED" : "FAILED", cycle ? "cycle" : missing ? "missing" : "remainder");
    return false;
}
} // namespace
bool networkWorldRunEngineAuthoritySmokeTest(EngineExecutionProbeCounts& counts)
{
    if (!nativeTransferOriginalAuthoritySmokeTest(counts)) return false;
    if (worldMode != NetworkLaunchMode::Join) return true;
    const char* mode = std::getenv("FALLOUT_INVENTORY_EVENT_BOUNDARY");
    if (mode != nullptr && std::string(mode) == "missing-transfer") return runMissingPositive(false);
    if (mode != nullptr && std::string(mode) == "missing-drop") return runMissingPositive(true);
    return runTransferBoundary();
}
} // namespace multiplayer
} // namespace fallout
