#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "multiplayer/acting_player_context.h"
#include "multiplayer/command_processor.h"
#include "multiplayer/entity_registry.h"
#include "multiplayer/local_session.h"
#include "multiplayer/local_player_context.h"
#include "multiplayer/loopback_transport.h"
#include "multiplayer/player_character_state.h"
#include "multiplayer/protocol.h"
#include "multiplayer/snapshot.h"
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

WorldSnapshot sampleSnapshot()
{
    WorldSnapshot snapshot;
    snapshot.lastIncludedEvent.value = 41;
    snapshot.phase = SessionPhase::Exploration;
    snapshot.phaseRevision = 7;
    snapshot.actors = {
        ActorSnapshot { EntityId { 2 }, kGuestPlayerId, 20102, 0, 3, 28 },
        ActorSnapshot { EntityId { 1 }, kHostPlayerId, 20100, 0, 1, 34 },
    };
    snapshot.doors = {
        DoorSnapshot { EntityId { 9 }, true, false, 5 },
    };
    return snapshot;
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

void testPlayerCharacterStateStore()
{
    EntityRegistry registry;
    PlayerCharacterStateStore players;
    TestObject hostActor;
    TestObject guestActor;
    TestObject replacementGuest;
    TestObject worldObject;

    EntityRegistrationResult host = registry.registerObject(asGameObject(hostActor), kHostPlayerId);
    EntityRegistrationResult guest = registry.registerObject(asGameObject(guestActor), kGuestPlayerId);
    EntityRegistrationResult world = registry.registerObject(asGameObject(worldObject));
    expect(static_cast<bool>(host) && static_cast<bool>(guest) && static_cast<bool>(world), "player state test entities register");

    PlayerCharacterState invalidPlayer;
    invalidPlayer.actorId = host.entityId;
    expect(players.registerPlayer(invalidPlayer, registry) == PlayerStateError::InvalidPlayerId, "player state rejects player ID zero");

    PlayerCharacterState invalidActor;
    invalidActor.id = kHostPlayerId;
    expect(players.registerPlayer(invalidActor, registry) == PlayerStateError::InvalidEntityId, "player state rejects actor ID zero");

    PlayerCharacterState missingActor;
    missingActor.id = kHostPlayerId;
    missingActor.actorId = EntityId { 999 };
    expect(players.registerPlayer(missingActor, registry) == PlayerStateError::ActorMissing, "player state requires a registered actor");

    PlayerCharacterState unownedActor;
    unownedActor.id = kHostPlayerId;
    unownedActor.actorId = world.entityId;
    expect(players.registerPlayer(unownedActor, registry) == PlayerStateError::ActorNotOwned, "player state requires matching actor ownership");

    PlayerCharacterState hostState;
    hostState.id = kHostPlayerId;
    hostState.actorId = host.entityId;
    hostState.ownership = PlayerOwnership::LocalControl;
    hostState.connection = ConnectionState::Local;
    hostState.build.baseStats[0] = 8;
    hostState.build.perkRanks[0] = 1;
    hostState.build.level = 4;
    expect(players.registerPlayer(hostState, registry) == PlayerStateError::None, "host player state registers");
    expect(players.registerPlayer(hostState, registry) == PlayerStateError::PlayerAlreadyRegistered, "player state rejects duplicate player IDs");

    PlayerCharacterState duplicateActor;
    duplicateActor.id = kGuestPlayerId;
    duplicateActor.actorId = host.entityId;
    expect(players.registerPlayer(duplicateActor, registry) == PlayerStateError::ActorAlreadyRegistered, "player state rejects duplicate actor bindings");

    PlayerCharacterState guestState;
    guestState.id = kGuestPlayerId;
    guestState.actorId = guest.entityId;
    guestState.ownership = PlayerOwnership::RemoteControl;
    guestState.connection = ConnectionState::Connected;
    guestState.build.baseStats[0] = 4;
    guestState.build.perkRanks[0] = 0;
    guestState.build.level = 2;
    expect(players.registerPlayer(guestState, registry) == PlayerStateError::None, "guest player state registers");
    expect(players.size() == 2 && players.bindingsMatch(registry), "player states match registry ownership");
    expect(players.find(kHostPlayerId)->build != players.find(kGuestPlayerId)->build, "host and guest retain distinct character builds");
    expect(players.findByActor(guest.entityId) == players.find(kGuestPlayerId), "player state resolves from actor identity");

    CharacterBuild updatedGuestBuild = players.find(kGuestPlayerId)->build;
    updatedGuestBuild.unspentSkillPoints = 12;
    expect(players.setBuild(kGuestPlayerId, updatedGuestBuild) == PlayerStateError::None, "guest build can be replaced");
    expect(players.setConnection(kGuestPlayerId, ConnectionState::Disconnected) == PlayerStateError::None, "guest connection state can change");
    expect(players.find(kGuestPlayerId)->build.unspentSkillPoints == 12, "guest build update is retained");
    expect(players.find(kGuestPlayerId)->connection == ConnectionState::Disconnected, "guest connection update is retained");
    expect(players.setBuild(PlayerId { 99 }, updatedGuestBuild) == PlayerStateError::PlayerNotFound, "unknown player build cannot be changed");

    expect(registry.rebindObject(guest.entityId, asGameObject(replacementGuest)) == EntityRegistryError::None, "guest object can be replaced beneath player state");
    expect(players.bindingsMatch(registry), "player binding survives object pointer replacement");
    expect(players.find(kGuestPlayerId)->build == updatedGuestBuild, "character build survives object pointer replacement");

    expect(players.unregisterPlayer(kGuestPlayerId) == PlayerStateError::None, "guest player state can be removed");
    expect(players.findByActor(guest.entityId) == nullptr, "removed actor binding no longer resolves");
    expect(players.unregisterPlayer(kGuestPlayerId) == PlayerStateError::PlayerNotFound, "player state cannot be removed twice");
    players.clear();
    expect(players.size() == 0, "clearing removes all player states");
}

void testActingPlayerContext()
{
    TestObject hostActor;
    TestObject guestActor;
    PlayerCharacterState host;
    host.id = kHostPlayerId;
    host.build.level = 3;
    PlayerCharacterState guest;
    guest.id = kGuestPlayerId;
    guest.build.level = 7;

    expect(actingPlayerState() == nullptr && actingPlayerActor() == nullptr, "acting-player context starts empty");
    expect(actingCharacterBuild() == nullptr, "no character build is active outside a scope");

    {
        ScopedActingPlayerContext hostContext(host, asGameObject(hostActor));
        expect(actingPlayerState() == &host, "acting-player scope exposes its player state");
        expect(actingPlayerActor() == asGameObject(hostActor), "acting-player scope exposes its actor");
        expect(actingCharacterBuildFor(asGameObject(hostActor)) == &host.build, "acting actor resolves its character build");
        expect(actingCharacterBuildFor(asGameObject(guestActor)) == nullptr, "another actor cannot use the active character build");
        actingCharacterBuild()->level = 4;

        {
            ScopedActingPlayerContext guestContext(guest, asGameObject(guestActor));
            expect(actingPlayerState() == &guest && actingCharacterBuild() == &guest.build, "nested scope replaces the acting player");
            expect(isActingPlayerActor(asGameObject(guestActor)), "nested scope recognizes the guest actor");
            actingCharacterBuild()->level = 8;
        }

        expect(actingPlayerState() == &host && actingPlayerActor() == asGameObject(hostActor), "leaving a nested scope restores the prior player");
        expect(host.build.level == 4 && guest.build.level == 8, "nested contexts mutate separate character builds");
    }

    expect(actingPlayerState() == nullptr && actingPlayerActor() == nullptr, "leaving the outer scope clears the acting player");
}

void testLocalPlayerContext()
{
    LocalSession session;
    TestObject hostActor;
    TestObject guestActor;
    TestObject replacementGuest;

    expect(bindLocalPlayer(session, kHostPlayerId) == LocalPlayerError::SessionInactive, "inactive session cannot bind a local player");
    expect(session.start(asGameObject(hostActor), asGameObject(guestActor)) == LocalSessionError::None, "local-player test session starts");
    expect(bindLocalPlayer(session, PlayerId { 99 }) == LocalPlayerError::PlayerMissing, "unknown player cannot become local");
    expect(bindLocalPlayer(session, kGuestPlayerId) == LocalPlayerError::None, "guest player can become the local presentation player");
    expect(localPlayerId() == kGuestPlayerId, "local-player binding exposes the selected player ID");
    expect(localPlayerState() == session.players().find(kGuestPlayerId), "local-player binding resolves registered state");
    expect(localPlayerActor() == asGameObject(guestActor), "local-player binding resolves the current actor object");
    expect(isLocalPlayerActor(asGameObject(guestActor)) && !isLocalPlayerActor(asGameObject(hostActor)), "local-player identity distinguishes the selected actor");

    {
        ScopedLocalPlayerContext localContext;
        expect(actingPlayerState() == localPlayerState(), "local presentation scope installs the selected player mechanically");
        expect(actingPlayerActor() == asGameObject(guestActor), "local presentation scope installs the selected actor mechanically");
    }
    expect(actingPlayerState() == nullptr, "local presentation scope restores the prior acting context");

    expect(session.transitionTo(SessionPhase::Loading) == LocalSessionError::None, "local-player test enters loading");
    expect(session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None, "local-player test enters exploration");
    expect(session.transitionTo(SessionPhase::Transition) == LocalSessionError::None, "local-player test enters map transition");
    expect(session.rebindPlayerActor(kGuestPlayerId, asGameObject(replacementGuest)) == LocalSessionError::None, "local player actor can be rebound");
    expect(localPlayerActor() == asGameObject(replacementGuest), "local-player lookup follows actor rebinding");

    session.stop();
    expect(!isValid(localPlayerId()) && localPlayerState() == nullptr && localPlayerActor() == nullptr, "stopping the bound session clears local presentation state");

    {
        LocalSession scopedSession;
        expect(scopedSession.start(asGameObject(hostActor), asGameObject(guestActor)) == LocalSessionError::None, "scoped local-player session starts");
        expect(bindLocalPlayer(scopedSession, kHostPlayerId) == LocalPlayerError::None, "scoped local-player session binds");
    }
    expect(localPlayerActor() == nullptr, "destroying the bound session clears local presentation state");
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
    expect(session.players().size() == 2, "local session creates both player states");
    expect(session.players().find(kHostPlayerId)->actorId == hostId, "host player state binds to the host actor");
    expect(session.players().find(kGuestPlayerId)->actorId == guestId, "guest player state binds to the guest actor");
    expect(session.players().bindingsMatch(session.entities()), "local session player states match registry ownership");
    expect(!isValid(session.playerActorId(PlayerId { 99 })), "unknown player has no actor entity ID");

    CharacterBuild guestBuild;
    guestBuild.baseStats[0] = 6;
    guestBuild.level = 3;
    expect(session.players().setBuild(kGuestPlayerId, guestBuild) == PlayerStateError::None, "local session accepts a guest character build");

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
    expect(session.players().bindingsMatch(session.entities()), "guest player state remains bound after object replacement");
    expect(session.players().find(kGuestPlayerId)->build == guestBuild, "guest character build survives object replacement");
    expect(session.rebindPlayerActor(PlayerId { 99 }, asGameObject(guestActor)) == LocalSessionError::InvalidPlayer, "unknown player actor cannot be rebound");

    session.stop();
    expect(!session.isActive(), "stopped local session is inactive");
    expect(session.entities().size() == 0, "stopping clears the local entity registry");
    expect(session.players().size() == 0, "stopping clears player character state");
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
        recordContext();
        return nextStatus;
    }

    CommandExecutionStatus useDoor(Object* actor, Object* target) override
    {
        doorCalls++;
        lastActor = actor;
        lastTarget = target;
        recordContext();
        return nextStatus;
    }

    void recordContext()
    {
        PlayerCharacterState* player = actingPlayerState();
        lastActingPlayerId = player != nullptr ? player->id : PlayerId {};
        lastContextActor = actingPlayerActor();
        lastBuild = actingCharacterBuild();
    }

    CommandExecutionStatus nextStatus = CommandExecutionStatus::Applied;
    int moveCalls = 0;
    int doorCalls = 0;
    Object* lastActor = nullptr;
    Object* lastTarget = nullptr;
    Object* lastContextActor = nullptr;
    CharacterBuild* lastBuild = nullptr;
    PlayerId lastActingPlayerId;
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
    expect(executor.lastActingPlayerId == kHostPlayerId, "host command executes with the host player context");
    expect(executor.lastContextActor == asGameObject(hostActor), "host context binds the commanded actor");
    expect(executor.lastBuild == &session.players().find(kHostPlayerId)->build, "host context exposes the registered character build");
    expect(actingPlayerState() == nullptr, "command execution does not leak its acting-player context");
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
    expect(executor.lastActingPlayerId == kGuestPlayerId, "guest command executes with the guest player context");
    expect(executor.lastContextActor == asGameObject(guestActor), "guest context binds the commanded actor");
    expect(executor.lastBuild == &session.players().find(kGuestPlayerId)->build, "guest context exposes the registered character build");
    expect(actingPlayerState() == nullptr, "guest command clears its context after execution");
    const DoorUseStartedEvent* doorEvent = usedDoor.event.has_value() ? std::get_if<DoorUseStartedEvent>(&usedDoor.event->payload) : nullptr;
    expect(doorEvent != nullptr && doorEvent->targetId == registeredDoor.entityId, "door event records the actor and target IDs");
    expect(usedDoor.event.has_value() && usedDoor.event->sequence == EventSequence { 2 }, "event sequence advances across players");

    executor.nextStatus = CommandExecutionStatus::InvalidAction;
    move.sequence.value = 2;
    AuthoritativeCommandResult invalid = processor.process(move, session, executor);
    expect(invalid.result.rejection == CommandRejection::InvalidAction, "executor can reject an impossible action");
    expect(!invalid.event.has_value(), "rejected action emits no authoritative event");
    expect(actingPlayerState() == nullptr, "invalid action does not leak its acting-player context");

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

void testSnapshotRoundTripAndRecovery()
{
    WorldSnapshot authoritative = sampleSnapshot();
    std::vector<std::uint8_t> packet;
    expect(encodeSnapshot(authoritative, packet) == SnapshotError::None, "valid snapshot encodes");
    expect(packet.size() == kSnapshotHeaderSize + 16 + 2 * 24 + 12, "snapshot packet declares a fixed-width payload");
    expect(packet[0] == 'F' && packet[1] == 'C' && packet[2] == 'M' && packet[3] == 'S', "snapshot magic uses network byte order");

    SnapshotDecodeResult decoded = decodeSnapshot(packet);
    expect(static_cast<bool>(decoded), "encoded snapshot decodes");
    expect(decoded.snapshot.lastIncludedEvent == EventSequence { 41 }, "snapshot keeps the last included event");
    expect(decoded.snapshot.phase == SessionPhase::Exploration && decoded.snapshot.phaseRevision == 7, "snapshot keeps session phase state");
    expect(decoded.snapshot.actors.size() == 2 && decoded.snapshot.actors[0].entityId == EntityId { 1 }, "decoded actors use canonical entity order");
    expect(decoded.snapshot.actors[1].tile == 20102 && decoded.snapshot.actors[1].hitPoints == 28, "snapshot keeps guest actor state");
    expect(decoded.snapshot.doors.size() == 1 && decoded.snapshot.doors[0].open, "snapshot keeps door state");

    SnapshotDigestResult authoritativeDigest = computeSnapshotDigest(authoritative);
    SnapshotDigestResult decodedDigest = computeSnapshotDigest(decoded.snapshot);
    expect(static_cast<bool>(authoritativeDigest) && authoritativeDigest.digest == decodedDigest.digest, "snapshot digest is stable across wire round trip and input order");
    expect(firstDivergentSection(authoritativeDigest.digest, decodedDigest.digest) == SnapshotSection::None, "matching snapshots report no divergent section");

    WorldSnapshot sessionDrift = decoded.snapshot;
    sessionDrift.phaseRevision++;
    SnapshotDigestResult sessionDriftDigest = computeSnapshotDigest(sessionDrift);
    expect(firstDivergentSection(authoritativeDigest.digest, sessionDriftDigest.digest) == SnapshotSection::Session, "phase drift reports the session section first");

    WorldSnapshot actorDrift = decoded.snapshot;
    actorDrift.actors[1].tile++;
    SnapshotDigestResult actorDriftDigest = computeSnapshotDigest(actorDrift);
    expect(firstDivergentSection(authoritativeDigest.digest, actorDriftDigest.digest) == SnapshotSection::Actors, "position drift reports the actor section");

    WorldSnapshot doorDrift = decoded.snapshot;
    doorDrift.doors[0].open = false;
    SnapshotDigestResult doorDriftDigest = computeSnapshotDigest(doorDrift);
    expect(firstDivergentSection(authoritativeDigest.digest, doorDriftDigest.digest) == SnapshotSection::Doors, "door drift reports the door section");

    SnapshotReplica replica;
    expect(replica.apply(actorDrift) == SnapshotError::None, "replica accepts locally drifted state");
    expect(replica.digest().digest != authoritativeDigest.digest, "drifted replica digest differs from the host");
    expect(replica.apply(decoded.snapshot) == SnapshotError::None, "replica applies authoritative recovery snapshot");
    expect(replica.digest().digest == authoritativeDigest.digest, "snapshot recovery reproduces the host digest");
    expect(replica.state().actors[1].tile == 20102, "snapshot recovery restores the guest position");

    WorldSnapshot invalidRecovery = decoded.snapshot;
    invalidRecovery.actors[0].tile = -1;
    expect(replica.apply(invalidRecovery) == SnapshotError::InvalidActorState, "replica rejects an invalid recovery snapshot");
    expect(replica.digest().digest == authoritativeDigest.digest, "failed recovery leaves replica state unchanged");
    replica.clear();
    expect(!replica.hasState(), "replica clear drops recovered state");

    std::vector<std::uint8_t> tooShort(packet.begin(), packet.begin() + kSnapshotHeaderSize - 1);
    expect(decodeSnapshot(tooShort).error == SnapshotError::PacketTooShort, "snapshot rejects a short header");

    std::vector<std::uint8_t> badMagic = packet;
    badMagic[0] = 0;
    expect(decodeSnapshot(badMagic).error == SnapshotError::InvalidMagic, "snapshot rejects invalid magic");

    std::vector<std::uint8_t> badVersion = packet;
    badVersion[4] = 0;
    badVersion[5] = kSnapshotVersion + 1;
    expect(decodeSnapshot(badVersion).error == SnapshotError::UnsupportedVersion, "snapshot rejects unsupported version");

    std::vector<std::uint8_t> corrupt = packet;
    corrupt.back() ^= 1;
    expect(decodeSnapshot(corrupt).error == SnapshotError::ChecksumMismatch, "snapshot rejects a corrupt payload");

    std::vector<std::uint8_t> corruptEventSequence = packet;
    corruptEventSequence[kSnapshotHeaderSize - 1] ^= 1;
    expect(decodeSnapshot(corruptEventSequence).error == SnapshotError::ChecksumMismatch, "snapshot checksum covers the last included event");

    std::vector<std::uint8_t> truncated = packet;
    truncated.pop_back();
    expect(decodeSnapshot(truncated).error == SnapshotError::TruncatedPayload, "snapshot rejects a truncated payload");

    std::vector<std::uint8_t> trailing = packet;
    trailing.push_back(0);
    expect(decodeSnapshot(trailing).error == SnapshotError::TrailingData, "snapshot rejects trailing data");

    WorldSnapshot duplicateEntity = authoritative;
    duplicateEntity.doors[0].entityId = duplicateEntity.actors[0].entityId;
    expect(validateSnapshot(duplicateEntity) == SnapshotError::DuplicateEntityId, "snapshot rejects duplicate entity IDs across sections");

    WorldSnapshot invalidActor = authoritative;
    invalidActor.actors[0].elevation = 3;
    expect(encodeSnapshot(invalidActor, packet) == SnapshotError::InvalidActorState, "snapshot rejects invalid actor coordinates before encoding");
    expect(packet.empty(), "failed snapshot encoding leaves no partial packet");

    WorldSnapshot invalidOwner = authoritative;
    invalidOwner.actors[0].ownerId.value = 99;
    expect(validateSnapshot(invalidOwner) == SnapshotError::InvalidPlayerId, "two-player snapshot rejects an unknown actor owner");
}

} // namespace
} // namespace multiplayer
} // namespace fallout

int main()
{
    fallout::multiplayer::testCoreTypes();
    fallout::multiplayer::testEntityRegistry();
    fallout::multiplayer::testEntityRegistryAcrossEngineLifecycles();
    fallout::multiplayer::testPlayerCharacterStateStore();
    fallout::multiplayer::testActingPlayerContext();
    fallout::multiplayer::testLocalPlayerContext();
    fallout::multiplayer::testProtocolRoundTrip();
    fallout::multiplayer::testProtocolRejectsInvalidPackets();
    fallout::multiplayer::testLoopbackTransport();
    fallout::multiplayer::testLocalSessionLifecycle();
    fallout::multiplayer::testAuthoritativeCommandProcessing();
    fallout::multiplayer::testSnapshotRoundTripAndRecovery();

    if (fallout::multiplayer::failures != 0) {
        std::cerr << fallout::multiplayer::failures << " test assertion(s) failed\n";
        return 1;
    }

    std::cout << "multiplayer core tests passed\n";
    return 0;
}
