#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "multiplayer/command_processor.h"
#include "multiplayer/entity_registry.h"
#include "multiplayer/local_session.h"
#include "multiplayer/loopback_transport.h"
#include "multiplayer/protocol.h"
#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {
namespace {

int failures = 0;

struct TestObject {
    int legacyId = -1;
    TestObject* owner = nullptr;
};

Object* asGameObject(TestObject& object)
{
    return reinterpret_cast<Object*>(&object);
}

void expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        failures++;
    }
}

ProtocolEnvelope sampleEnvelope()
{
    ProtocolEnvelope envelope;
    envelope.kind = MessageKind::Command;
    envelope.sessionId.value = 0x0102030405060708ULL;
    envelope.sequence = 0x1112131415161718ULL;
    envelope.payload = { 0x21, 0x22, 0x23 };
    return envelope;
}

void testCoreTypes()
{
    expect(PlayerId { 1 } == PlayerId { 1 }, "equal player IDs compare equal");
    expect(PlayerId { 1 } != PlayerId { 2 }, "different player IDs compare unequal");
    expect(EntityIdHash {}(EntityId { 42 }) == 42, "entity ID hash uses its numeric value");

    GameCommand command;
    command.sequence.value = 9;
    command.playerId.value = 1;
    command.actorId.value = 100;
    command.expectedPhase = SessionPhase::Exploration;
    command.expectedPhaseRevision = 3;
    command.payload = MoveCommand { 12345, 1 };

    const MoveCommand* move = std::get_if<MoveCommand>(&command.payload);
    expect(move != nullptr, "game command keeps typed movement arguments");
    expect(move != nullptr && move->destinationTile == 12345, "movement destination survives assignment");
}

void testEntityRegistry()
{
    EntityRegistry registry;
    TestObject hostActor;
    TestObject guestActor;
    TestObject clone;

    EntityRegistrationResult host = registry.registerObject(asGameObject(hostActor), PlayerId { 1 });
    EntityRegistrationResult guest = registry.registerObject(asGameObject(guestActor), PlayerId { 2 });
    EntityRegistrationResult cloned = registry.registerObject(asGameObject(clone));

    expect(static_cast<bool>(host) && host.entityId == EntityId { 1 }, "first object receives entity ID 1");
    expect(static_cast<bool>(guest) && guest.entityId == EntityId { 2 }, "second object receives entity ID 2");
    expect(static_cast<bool>(cloned) && cloned.entityId == EntityId { 3 }, "clone receives a distinct entity ID");
    expect(registry.size() == 3, "registry reports registered entity count");
    expect(registry.findObject(guest.entityId) == asGameObject(guestActor), "entity ID resolves to its object");
    expect(registry.findEntity(asGameObject(guestActor)) == guest.entityId, "object resolves to its entity ID");
    expect(registry.isOwnedBy(host.entityId, PlayerId { 1 }), "host actor ownership is recorded");
    expect(!registry.isOwnedBy(host.entityId, PlayerId { 2 }), "ownership rejects another player");
    expect(!registry.ownerOf(cloned.entityId).has_value(), "unowned entity has no player");

    expect(registry.setOwner(cloned.entityId, PlayerId { 2 }) == EntityRegistryError::None, "owner can be assigned");
    expect(registry.isOwnedBy(cloned.entityId, PlayerId { 2 }), "assigned owner is returned");
    expect(registry.clearOwner(cloned.entityId) == EntityRegistryError::None, "owner can be cleared");
    expect(!registry.ownerOf(cloned.entityId).has_value(), "cleared entity is unowned");

    expect(registry.registerObject(nullptr).error == EntityRegistryError::NullObject, "null object is rejected");
    expect(registry.registerObject(asGameObject(hostActor)).error == EntityRegistryError::ObjectAlreadyRegistered, "same object cannot register twice");
    expect(registry.rebindObject(guest.entityId, asGameObject(hostActor)) == EntityRegistryError::ObjectAlreadyRegistered, "rebind cannot steal another entity's object");
    expect(registry.setOwner(EntityId { 999 }, PlayerId { 1 }) == EntityRegistryError::EntityNotFound, "unknown entity cannot gain an owner");
    expect(registry.setOwner(host.entityId, PlayerId {}) == EntityRegistryError::InvalidPlayerId, "player ID zero is rejected");

    expect(registry.unregisterEntity(cloned.entityId) == EntityRegistryError::None, "entity can be unregistered");
    expect(registry.unregisterEntity(cloned.entityId) == EntityRegistryError::EntityNotFound, "entity cannot be unregistered twice");
    expect(!registry.contains(cloned.entityId), "unregistered entity is absent");
    expect(!registry.findEntity(asGameObject(clone)).has_value(), "unregistered object is absent");
}

void testEntityRegistryAcrossEngineLifecycles()
{
    EntityRegistry registry;
    TestObject inventoryOwner;
    TestObject inventoryItem;
    inventoryItem.legacyId = 17;

    EntityRegistrationResult registered = registry.registerObject(asGameObject(inventoryItem), PlayerId { 2 });
    expect(static_cast<bool>(registered), "inventory item registers");

    inventoryItem.owner = &inventoryOwner;
    expect(registry.findEntity(asGameObject(inventoryItem)) == registered.entityId, "inventory move keeps entity identity");

    inventoryItem.legacyId = 20001;
    expect(registry.findEntity(asGameObject(inventoryItem)) == registered.entityId, "legacy object ID rewrite does not change entity identity");

    TestObject loadedItem;
    loadedItem.legacyId = 17;
    expect(registry.rebindObject(registered.entityId, asGameObject(loadedItem)) == EntityRegistryError::None, "map load can rebind an entity to a new object");
    expect(registry.findObject(registered.entityId) == asGameObject(loadedItem), "rebound entity resolves to loaded object");
    expect(!registry.findEntity(asGameObject(inventoryItem)).has_value(), "old object pointer is removed after rebind");
    expect(registry.isOwnedBy(registered.entityId, PlayerId { 2 }), "rebind preserves ownership");

    registry.clear();
    expect(registry.size() == 0, "session clear removes all entities");

    TestObject restoredActor;
    EntityId savedEntityId { 73 };
    expect(registry.restoreObject(savedEntityId, asGameObject(restoredActor), PlayerId { 2 }) == EntityRegistryError::None, "save metadata restores an entity ID");
    expect(registry.isOwnedBy(savedEntityId, PlayerId { 2 }), "save metadata restores ownership");

    TestObject newObject;
    EntityRegistrationResult afterRestore = registry.registerObject(asGameObject(newObject));
    expect(afterRestore.entityId == EntityId { 74 }, "new IDs advance beyond restored IDs");

    TestObject duplicateIdObject;
    expect(registry.restoreObject(savedEntityId, asGameObject(duplicateIdObject)) == EntityRegistryError::EntityIdInUse, "restored entity ID collision is rejected");
    expect(registry.restoreObject(EntityId { 75 }, asGameObject(restoredActor)) == EntityRegistryError::ObjectAlreadyRegistered, "restored object collision is rejected");
    expect(registry.restoreObject(EntityId {}, asGameObject(duplicateIdObject)) == EntityRegistryError::InvalidEntityId, "entity ID zero is rejected");

    registry.clear();
    TestObject lastObject;
    TestObject exhaustedObject;
    EntityId lastEntityId { std::numeric_limits<std::uint32_t>::max() };
    expect(registry.restoreObject(lastEntityId, asGameObject(lastObject)) == EntityRegistryError::None, "largest entity ID can be restored");
    expect(registry.registerObject(asGameObject(exhaustedObject)).error == EntityRegistryError::EntityIdsExhausted, "entity ID allocation does not wrap");
}

void testProtocolRoundTrip()
{
    ProtocolEnvelope envelope = sampleEnvelope();
    std::vector<std::uint8_t> packet;

    expect(encodeEnvelope(envelope, packet) == ProtocolError::None, "protocol envelope encodes");
    expect(packet.size() == kProtocolHeaderSize + envelope.payload.size(), "encoded packet has the expected size");
    expect(packet[0] == 'F' && packet[1] == 'C' && packet[2] == 'M' && packet[3] == 'P', "protocol magic uses network byte order");

    ProtocolDecodeResult decoded = decodeEnvelope(packet);
    expect(static_cast<bool>(decoded), "encoded envelope decodes");
    expect(decoded.envelope.version == envelope.version, "protocol version round trips");
    expect(decoded.envelope.kind == envelope.kind, "message kind round trips");
    expect(decoded.envelope.sessionId == envelope.sessionId, "session ID round trips");
    expect(decoded.envelope.sequence == envelope.sequence, "sequence number round trips");
    expect(decoded.envelope.payload == envelope.payload, "payload round trips");
}

void testProtocolRejectsInvalidPackets()
{
    ProtocolEnvelope envelope = sampleEnvelope();
    std::vector<std::uint8_t> packet;
    encodeEnvelope(envelope, packet);

    std::vector<std::uint8_t> tooShort(packet.begin(), packet.begin() + kProtocolHeaderSize - 1);
    expect(decodeEnvelope(tooShort).error == ProtocolError::PacketTooShort, "short header is rejected");

    std::vector<std::uint8_t> badMagic = packet;
    badMagic[0] = 0;
    expect(decodeEnvelope(badMagic).error == ProtocolError::InvalidMagic, "bad magic is rejected");

    std::vector<std::uint8_t> badVersion = packet;
    badVersion[4] = 0;
    badVersion[5] = kProtocolVersion + 1;
    expect(decodeEnvelope(badVersion).error == ProtocolError::UnsupportedVersion, "unsupported version is rejected");

    std::vector<std::uint8_t> unknownKind = packet;
    unknownKind[6] = 0x7F;
    unknownKind[7] = 0xFF;
    expect(decodeEnvelope(unknownKind).error == ProtocolError::UnknownMessageKind, "unknown message kind is rejected");

    std::vector<std::uint8_t> truncated = packet;
    truncated.pop_back();
    expect(decodeEnvelope(truncated).error == ProtocolError::TruncatedPayload, "truncated payload is rejected");

    std::vector<std::uint8_t> trailing = packet;
    trailing.push_back(0);
    expect(decodeEnvelope(trailing).error == ProtocolError::TrailingData, "trailing data is rejected");

    ProtocolEnvelope oversized = sampleEnvelope();
    oversized.payload.resize(kMaxProtocolPayloadSize + 1);
    expect(encodeEnvelope(oversized, packet) == ProtocolError::PayloadTooLarge, "oversized payload is rejected before encoding");
    expect(packet.empty(), "failed encoding leaves no partial packet");
}

void testLoopbackTransport()
{
    LoopbackTransportPair pair = createLoopbackTransportPair();
    expect(pair.first->isConnected() && pair.second->isConnected(), "loopback endpoints start connected");
    expect(!pair.first->receive().has_value(), "empty loopback queue has no packet");

    Packet first = { 1, 2, 3 };
    Packet second = { 4, 5 };
    expect(pair.first->send(first) == TransportSendResult::Sent, "first endpoint sends to second");
    expect(pair.first->send(second) == TransportSendResult::Sent, "loopback accepts a second packet");

    std::optional<Packet> receivedFirst = pair.second->receive();
    std::optional<Packet> receivedSecond = pair.second->receive();
    expect(receivedFirst.has_value() && *receivedFirst == first, "loopback preserves the first packet");
    expect(receivedSecond.has_value() && *receivedSecond == second, "loopback preserves FIFO order");

    Packet reply = { 9 };
    expect(pair.second->send(reply) == TransportSendResult::Sent, "loopback is bidirectional");
    std::optional<Packet> receivedReply = pair.first->receive();
    expect(receivedReply.has_value() && *receivedReply == reply, "first endpoint receives the reply");

    pair.second->close();
    expect(!pair.first->isConnected(), "closing one endpoint disconnects its peer");
    expect(pair.first->send(Packet { 7 }) == TransportSendResult::Disconnected, "send fails after peer closes");
}

void testLocalSessionLifecycle()
{
    LocalSession session;
    TestObject hostActor;
    TestObject guestActor;
    TestObject replacementGuest;

    expect(!session.isActive(), "local session starts inactive");
    expect(session.phaseRevision() == 0, "inactive session has no phase revision");
    expect(session.start(nullptr, asGameObject(guestActor)) == LocalSessionError::NullActor, "local session rejects a null host actor");
    expect(session.start(asGameObject(hostActor), asGameObject(hostActor)) == LocalSessionError::SameActor, "local session requires distinct actors");
    expect(session.start(asGameObject(hostActor), asGameObject(guestActor)) == LocalSessionError::None, "local session starts with two actors");
    expect(session.isActive(), "started local session is active");
    expect(session.phase() == SessionPhase::Lobby && session.phaseRevision() == 1, "local session starts in the first lobby revision");
    expect(session.start(asGameObject(hostActor), asGameObject(guestActor)) == LocalSessionError::AlreadyActive, "active local session cannot start twice");

    EntityId hostId = session.playerActorId(kHostPlayerId);
    EntityId guestId = session.playerActorId(kGuestPlayerId);
    expect(isValid(hostId) && isValid(guestId) && hostId != guestId, "players receive distinct actor entity IDs");
    expect(session.owns(kHostPlayerId, hostId), "host owns the host actor");
    expect(session.owns(kGuestPlayerId, guestId), "guest owns the guest actor");
    expect(!isValid(session.playerActorId(PlayerId { 99 })), "unknown player has no actor entity ID");

    expect(session.transitionTo(SessionPhase::Combat) == LocalSessionError::InvalidTransition, "lobby cannot jump directly to combat");
    expect(session.phaseRevision() == 1, "rejected transition does not change phase revision");
    expect(session.transitionTo(SessionPhase::Loading) == LocalSessionError::None, "lobby can enter loading");
    expect(session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None, "loading can enter exploration");
    expect(session.phaseRevision() == 3, "accepted transitions advance the phase revision");
    expect(session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None, "repeating the current phase is harmless");
    expect(session.phaseRevision() == 3, "repeating the current phase does not advance its revision");

    Transport* hostTransport = session.transportFor(kHostPlayerId);
    Transport* guestTransport = session.transportFor(kGuestPlayerId);
    expect(hostTransport != nullptr && guestTransport != nullptr, "both players have loopback transport endpoints");
    expect(hostTransport->send(Packet { 4, 2 }) == TransportSendResult::Sent, "host can send through the local session transport");
    std::optional<Packet> packet = guestTransport->receive();
    expect(packet.has_value() && *packet == Packet({ 4, 2 }), "guest receives the host packet");

    expect(session.transitionTo(SessionPhase::Transition) == LocalSessionError::None, "exploration can enter a map transition");
    expect(session.rebindPlayerActor(kGuestPlayerId, asGameObject(replacementGuest)) == LocalSessionError::None, "map transition can replace the guest object");
    expect(session.playerActorId(kGuestPlayerId) == guestId, "guest entity ID survives object replacement");
    expect(session.entities().findObject(guestId) == asGameObject(replacementGuest), "guest entity resolves to the replacement object");
    expect(session.owns(kGuestPlayerId, guestId), "guest ownership survives object replacement");
    expect(session.rebindPlayerActor(PlayerId { 99 }, asGameObject(guestActor)) == LocalSessionError::InvalidPlayer, "unknown player actor cannot be rebound");

    session.stop();
    expect(!session.isActive(), "stopped local session is inactive");
    expect(session.entities().size() == 0, "stopping clears the local entity registry");
    expect(session.transportFor(kHostPlayerId) == nullptr, "stopping removes local transports");
    expect(session.transitionTo(SessionPhase::Loading) == LocalSessionError::NotActive, "inactive session cannot change phase");
}

class RecordingCommandExecutor : public CommandExecutor {
public:
    CommandExecutionStatus move(Object* actor, const MoveCommand& command) override
    {
        moveCalls++;
        lastActor = actor;
        lastMove = command;
        return nextStatus;
    }

    CommandExecutionStatus useDoor(Object* actor, Object* target) override
    {
        doorCalls++;
        lastActor = actor;
        lastTarget = target;
        return nextStatus;
    }

    CommandExecutionStatus nextStatus = CommandExecutionStatus::Applied;
    int moveCalls = 0;
    int doorCalls = 0;
    Object* lastActor = nullptr;
    Object* lastTarget = nullptr;
    MoveCommand lastMove;
};

void testAuthoritativeCommandProcessing()
{
    TestObject hostActor;
    TestObject guestActor;
    TestObject door;
    TestObject laterWorldObject;
    LocalSession session;
    expect(session.start(asGameObject(hostActor), asGameObject(guestActor)) == LocalSessionError::None, "command test session starts");
    expect(session.transitionTo(SessionPhase::Loading) == LocalSessionError::None, "command test session enters loading");
    expect(session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None, "command test session enters exploration");

    EntityRegistrationResult registeredDoor = session.registerWorldObject(asGameObject(door));
    expect(static_cast<bool>(registeredDoor), "door receives a world entity ID");
    expect(session.registerWorldObject(asGameObject(door)).entityId == registeredDoor.entityId, "registering the same world object returns its entity ID");

    CommandProcessor processor;
    RecordingCommandExecutor executor;

    GameCommand move;
    move.sequence.value = 1;
    move.playerId = kHostPlayerId;
    move.actorId = session.playerActorId(kHostPlayerId);
    move.expectedPhase = SessionPhase::Exploration;
    move.expectedPhaseRevision = session.phaseRevision();
    move.payload = MoveCommand { 12345, 1, true };

    AuthoritativeCommandResult moved = processor.process(move, session, executor);
    expect(moved.result.status == CommandStatus::Accepted, "owned movement command is accepted");
    expect(moved.result.rejection == CommandRejection::None, "accepted movement has no rejection reason");
    expect(moved.result.firstEventSequence == EventSequence { 1 } && moved.result.eventCount == 1, "accepted movement names its authoritative event");
    expect(executor.moveCalls == 1 && executor.lastActor == asGameObject(hostActor), "movement executes once for the owned actor");
    const ActorMovementStartedEvent* moveEvent = moved.event.has_value() ? std::get_if<ActorMovementStartedEvent>(&moved.event->payload) : nullptr;
    expect(moveEvent != nullptr && moveEvent->destinationTile == 12345 && moveEvent->running, "movement event records the accepted destination and gait");

    AuthoritativeCommandResult duplicate = processor.process(move, session, executor);
    expect(duplicate.result.status == CommandStatus::Accepted, "duplicate command returns its prior result");
    expect(executor.moveCalls == 1, "duplicate command does not execute twice");
    expect(duplicate.event.has_value() && duplicate.event->sequence == EventSequence { 1 }, "duplicate command returns the original event");

    GameCommand stolen = move;
    stolen.sequence.value = 1;
    stolen.playerId = kGuestPlayerId;
    AuthoritativeCommandResult notOwner = processor.process(stolen, session, executor);
    expect(notOwner.result.rejection == CommandRejection::NotOwner, "player cannot command another player's actor");
    expect(executor.moveCalls == 1, "ownership rejection does not reach the executor");

    GameCommand useDoor;
    useDoor.sequence.value = 2;
    useDoor.playerId = kGuestPlayerId;
    useDoor.actorId = session.playerActorId(kGuestPlayerId);
    useDoor.expectedPhase = SessionPhase::Exploration;
    useDoor.expectedPhaseRevision = session.phaseRevision();
    useDoor.payload = InteractCommand { registeredDoor.entityId };
    AuthoritativeCommandResult usedDoor = processor.process(useDoor, session, executor);
    expect(usedDoor.result.status == CommandStatus::Accepted, "owned door command is accepted");
    expect(executor.doorCalls == 1 && executor.lastTarget == asGameObject(door), "door command resolves the authoritative target object");
    const DoorUseStartedEvent* doorEvent = usedDoor.event.has_value() ? std::get_if<DoorUseStartedEvent>(&usedDoor.event->payload) : nullptr;
    expect(doorEvent != nullptr && doorEvent->targetId == registeredDoor.entityId, "door event records the actor and target IDs");
    expect(usedDoor.event.has_value() && usedDoor.event->sequence == EventSequence { 2 }, "event sequence advances across players");

    executor.nextStatus = CommandExecutionStatus::InvalidAction;
    move.sequence.value = 2;
    AuthoritativeCommandResult invalid = processor.process(move, session, executor);
    expect(invalid.result.rejection == CommandRejection::InvalidAction, "executor can reject an impossible action");
    expect(!invalid.event.has_value(), "rejected action emits no authoritative event");

    move.sequence.value = 4;
    AuthoritativeCommandResult gap = processor.process(move, session, executor);
    expect(gap.result.rejection == CommandRejection::Stale, "command sequence gap is rejected");
    expect(executor.moveCalls == 2, "sequence gap does not reach the executor");

    executor.nextStatus = CommandExecutionStatus::Applied;
    move.sequence.value = 3;
    move.payload = InteractCommand { EntityId { 999 } };
    AuthoritativeCommandResult missing = processor.process(move, session, executor);
    expect(missing.result.rejection == CommandRejection::MissingEntity, "unknown interaction target is rejected");

    AuthoritativeCommandResult olderDuplicate = processor.process(GameCommand {
        CommandSequence { 1 },
        kHostPlayerId,
        session.playerActorId(kHostPlayerId),
        SessionPhase::Exploration,
        session.phaseRevision(),
        MoveCommand { 12345, 1, true },
    }, session, executor);
    expect(olderDuplicate.result.status == CommandStatus::Accepted, "older cached command returns its prior result");
    expect(executor.moveCalls == 2, "older cached command does not execute twice");

    expect(session.transitionTo(SessionPhase::Combat) == LocalSessionError::None, "command test session enters combat");
    move.sequence.value = 4;
    move.expectedPhase = SessionPhase::Combat;
    move.expectedPhaseRevision = session.phaseRevision();
    move.payload = MoveCommand { 100, 0, false };
    AuthoritativeCommandResult wrongPhase = processor.process(move, session, executor);
    expect(wrongPhase.result.rejection == CommandRejection::WrongPhase, "exploration command is rejected during combat");

    std::uint32_t combatRevision = session.phaseRevision();
    expect(session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None, "command test session returns to exploration");
    move.sequence.value = 5;
    move.expectedPhase = SessionPhase::Exploration;
    move.expectedPhaseRevision = combatRevision;
    AuthoritativeCommandResult stalePhase = processor.process(move, session, executor);
    expect(stalePhase.result.rejection == CommandRejection::Stale, "stale phase revision is rejected");

    session.clearWorldEntities();
    expect(session.entities().size() == 2, "world reset preserves player actors only");
    expect(session.entities().findObject(registeredDoor.entityId) == nullptr, "world reset removes the old door pointer");
    EntityRegistrationResult later = session.registerWorldObject(asGameObject(laterWorldObject));
    expect(later.entityId.value > registeredDoor.entityId.value, "world entity IDs are not reused after a reset");
}

} // namespace
} // namespace multiplayer
} // namespace fallout

int main()
{
    fallout::multiplayer::testCoreTypes();
    fallout::multiplayer::testEntityRegistry();
    fallout::multiplayer::testEntityRegistryAcrossEngineLifecycles();
    fallout::multiplayer::testProtocolRoundTrip();
    fallout::multiplayer::testProtocolRejectsInvalidPackets();
    fallout::multiplayer::testLoopbackTransport();
    fallout::multiplayer::testLocalSessionLifecycle();
    fallout::multiplayer::testAuthoritativeCommandProcessing();

    if (fallout::multiplayer::failures != 0) {
        std::cerr << fallout::multiplayer::failures << " test assertion(s) failed\n";
        return 1;
    }

    std::cout << "multiplayer core tests passed\n";
    return 0;
}
