#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "game/object_types.h"
#include "multiplayer/acting_player_context.h"
#include "multiplayer/character_lobby.h"
#include "multiplayer/combat_turn_controller.h"
#include "multiplayer/command_processor.h"
#include "multiplayer/connection_handshake.h"
#include "multiplayer/content_manifest.h"
#include "multiplayer/dialogue_vote_controller.h"
#include "multiplayer/direct_trade_controller.h"
#include "multiplayer/entity_registry.h"
#include "multiplayer/gameplay_wire.h"
#include "multiplayer/local_session.h"
#include "multiplayer/local_player_context.h"
#include "multiplayer/loot_policy.h"
#include "multiplayer/loopback_transport.h"
#include "multiplayer/network_bootstrap.h"
#include "multiplayer/network_lobby.h"
#include "multiplayer/player_character_state.h"
#include "multiplayer/protocol.h"
#include "multiplayer/save_sidecar.h"
#include "multiplayer/session_recovery.h"
#include "multiplayer/snapshot.h"
#include "multiplayer/tcp_transport.h"
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

CharacterCreationSheet sampleCharacterSheet(PlayerId playerId, const std::string& name)
{
    CharacterCreationSheet sheet;
    sheet.playerId = playerId;
    sheet.name = name;
    sheet.primaryStats = { 5, 5, 5, 5, 5, 5, 10 };
    sheet.taggedSkills = { SKILL_SMALL_GUNS, SKILL_FIRST_AID, SKILL_SPEECH };
    return sheet;
}

void submitBothCharacterSheets(LocalSession& session)
{
    expect(session.submitCharacterSheet(sampleCharacterSheet(kHostPlayerId, "Host")) == CharacterLobbyError::None, "host character sheet is accepted");
    expect(session.submitCharacterSheet(sampleCharacterSheet(kGuestPlayerId, "Guest")) == CharacterLobbyError::None, "guest character sheet is accepted");
}

WorldSnapshot sampleSnapshot()
{
    WorldSnapshot snapshot;
    snapshot.lastIncludedEvent.value = 41;
    snapshot.phase = SessionPhase::Exploration;
    snapshot.phaseRevision = 7;
    snapshot.gameTime = 302400;
    snapshot.worldMap.firstVisits = 5;
    snapshot.worldMap.specialEncounters = 2;
    snapshot.worldMap.town = TOWN_SHADY_SANDS;
    snapshot.worldMap.section = 1;
    snapshot.worldMap.x = 1075;
    snapshot.worldMap.y = 75;
    snapshot.worldMap.grid[42] = 2;
    snapshot.worldMap.knownTownEntrances[14] = 1;
    snapshot.actors = {
        ActorSnapshot { EntityId { 2 }, kGuestPlayerId, 20102, 0, 3, 28 },
        ActorSnapshot { EntityId { 1 }, kHostPlayerId, 20100, 0, 1, 34 },
    };
    snapshot.actors[0].build.level = 2;
    snapshot.actors[0].build.experience = 2500;
    snapshot.actors[0].build.unspentSkillPoints = 7;
    snapshot.actors[0].build.prototypeFlags = 1 << 3; // PC_FLAG_LEVEL_UP_AVAILABLE.
    snapshot.actors[1].build.experience = 125;
    snapshot.actors[0].fid = 0x01000002;
    snapshot.actors[0].frame = 2;
    snapshot.actors[0].objectFlags = 0x10;
    snapshot.actors[0].combatManeuver = 1;
    snapshot.actors[0].whoHitMeId = EntityId { 12 };
    snapshot.critters = {
        CritterSnapshot { EntityId { 12 }, 0x01000023, 20106, 0, 4, 6, 7, 0, 1 },
    };
    snapshot.critters[0].fid = 0x01000004;
    snapshot.critters[0].frame = 3;
    snapshot.critters[0].damageLastTurn = 5;
    snapshot.critters[0].whoHitMeId = EntityId { 2 };
    snapshot.doors = {
        DoorSnapshot { EntityId { 9 }, true, false, 5 },
    };
    snapshot.scenery = {
        ScenerySnapshot { EntityId { 13 }, 0x02000001, 0x02000002, 20108, 0, 2, 1, 0x10, 2, 1000, 7, 8 },
    };
    snapshot.items = {
        ItemSnapshot { EntityId { 10 }, EntityId { 1 }, -1, -1, 2, ItemDescriptor { 40, 0, 0, 0 } },
        ItemSnapshot { EntityId { 11 }, {}, 20104, 0, 1, ItemDescriptor { 41, 0, 0, 0 } },
    };
    snapshot.gameGlobalVariables = { 7, -9 };
    snapshot.mapGlobalVariables = { 11 };
    snapshot.mapLocalVariables = { 100, 101, 102 };
    TimedEventSnapshot scriptEvent;
    scriptEvent.time = 302500;
    scriptEvent.eventType = 3;
    scriptEvent.payloadCount = 2;
    scriptEvent.payload[0] = 0x01000042;
    scriptEvent.payload[1] = -7;
    TimedEventSnapshot poisonEvent;
    poisonEvent.time = 302600;
    poisonEvent.eventType = 5;
    poisonEvent.ownerId = EntityId { 1 };
    snapshot.timedEvents = { scriptEvent, poisonEvent };
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

void testCombatTurnController()
{
    const PlayerId thirdPlayer { 3 };
    const EntityId hostActor { 10 };
    const EntityId guestActor { 11 };
    const EntityId aiActor { 12 };
    const EntityId thirdActor { 13 };
    CombatTurnController host;
    CombatTurnController replica;
    std::vector<CombatTurnEntry> order {
        { hostActor, kHostPlayerId },
        { guestActor, kGuestPlayerId },
        { aiActor, std::nullopt },
        { thirdActor, thirdPlayer },
    };
    expect(host.begin(order, 100, 30) == CombatTurnResult::Accepted,
        "four-actor initiative begins");
    expect(replica.begin(order, 100, 30) == CombatTurnResult::Accepted,
        "replica accepts the same initiative");
    expect(host.current()->actorId == hostActor && host.revision() == 1
            && host.deadline() == 130,
        "host owns first turn with deadline");

    expect(host.endPlayerTurn(kGuestPlayerId, hostActor, 1, 105)
            == CombatTurnResult::OutOfTurn,
        "wrong player cannot end host turn");
    expect(host.endPlayerTurn(kHostPlayerId, guestActor, 1, 105)
            == CombatTurnResult::OutOfTurn,
        "wrong actor cannot end host turn");
    expect(host.expirePlayerTurn(kGuestPlayerId, hostActor, 1, 130)
            == CombatTurnResult::OutOfTurn,
        "timeout cannot name a different owner");
    expect(host.expirePlayerTurn(kHostPlayerId, hostActor, 1, 129)
            == CombatTurnResult::NotExpired,
        "turn cannot expire before deadline");
    expect(host.current()->actorId == hostActor && host.revision() == 1,
        "rejected commands do not change turn state");

    expect(host.endPlayerTurn(kHostPlayerId, hostActor, 1, 105)
            == CombatTurnResult::Accepted,
        "host advances to guest");
    expect(replica.endPlayerTurn(kHostPlayerId, hostActor, 1, 105)
            == CombatTurnResult::Accepted,
        "replica follows host advancement");
    expect(host.endPlayerTurn(kHostPlayerId, hostActor, 1, 110)
            == CombatTurnResult::StaleTurn,
        "replayed turn cannot advance twice");
    expect(host.endAiTurn(guestActor, 2, 110) == CombatTurnResult::OutOfTurn,
        "player actor cannot be advanced as AI");
    expect(host.endPlayerTurn(kGuestPlayerId, guestActor, 2, 110)
            == CombatTurnResult::Accepted,
        "guest advances to AI");
    expect(replica.endPlayerTurn(kGuestPlayerId, guestActor, 2, 110)
            == CombatTurnResult::Accepted,
        "replica follows guest advancement");
    expect(host.endAiTurn(aiActor, 3, 112) == CombatTurnResult::Accepted,
        "host AI advances to third player");
    expect(replica.endAiTurn(aiActor, 3, 112) == CombatTurnResult::Accepted,
        "replica follows AI advancement");

    expect(host.passDisconnectedPlayer(thirdPlayer, thirdActor, 4, 113)
            == CombatTurnResult::StillConnected,
        "connected third player cannot be passed as disconnected");
    host.setConnected(thirdPlayer, false);
    replica.setConnected(thirdPlayer, false);
    expect(host.endPlayerTurn(thirdPlayer, thirdActor, 4, 113)
            == CombatTurnResult::Disconnected,
        "disconnected owner cannot issue end-turn command");
    expect(host.passDisconnectedPlayer(thirdPlayer, thirdActor, 4, 113)
            == CombatTurnResult::Accepted,
        "disconnected third player is passed by owner identity");
    expect(replica.passDisconnectedPlayer(thirdPlayer, thirdActor, 4, 113)
            == CombatTurnResult::Accepted,
        "replica follows disconnect pass");
    expect(host.current()->actorId == replica.current()->actorId
            && host.revision() == replica.revision()
            && host.round() == replica.round()
            && host.round() == 2,
        "three players and AI retain identical initiative across rounds");
    expect(host.expirePlayerTurn(kHostPlayerId, hostActor, 5, 143)
            == CombatTurnResult::Accepted,
        "owner turn expires at deadline");
    expect(host.current()->actorId == guestActor,
        "expired turn advances to next owner");

    const std::uint64_t revision = host.revision();
    expect(host.begin({ { hostActor, kHostPlayerId }, { hostActor, thirdPlayer } },
               200, 30)
            == CombatTurnResult::InvalidOrder,
        "duplicate actor initiative is rejected");
    expect(host.revision() == revision,
        "invalid initiative does not replace active turn");
    host.stop();
    expect(host.endPlayerTurn(kGuestPlayerId, guestActor, revision, 200)
            == CombatTurnResult::Inactive,
        "stopped combat rejects turn commands");

    CombatTurnController incapacitated;
    expect(incapacitated.begin({ { thirdActor, thirdPlayer } }, 100, 30)
            == CombatTurnResult::Accepted,
        "incapacitated owner fixture starts");
    incapacitated.setConnected(thirdPlayer, false);
    expect(incapacitated.endPlayerTurn(thirdPlayer, thirdActor, 1, 101)
            == CombatTurnResult::Disconnected
            && incapacitated.passPlayerTurn(thirdPlayer, thirdActor, 1, 101)
                == CombatTurnResult::Accepted,
        "host may pass an incapacitated owner without accepting disconnected input");
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
    hostState.name = "Host";
    hostState.build.baseStats[0] = 8;
    hostState.build.perkRanks[0] = 1;
    hostState.build.level = 4;
    expect(players.registerPlayer(hostState, registry) == PlayerStateError::None, "host player state registers");
    expect(players.find(kHostPlayerId)->name == "Host", "player state retains the character name");
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
    expect(players.playerIds() == std::vector<PlayerId> { kHostPlayerId, kGuestPlayerId }, "player IDs enumerate deterministically");
    expect(players.find(kHostPlayerId)->build != players.find(kGuestPlayerId)->build, "host and guest retain distinct character builds");
    expect(players.findByActor(guest.entityId) == players.find(kGuestPlayerId), "player state resolves from actor identity");

    CharacterBuild updatedGuestBuild = players.find(kGuestPlayerId)->build;
    updatedGuestBuild.unspentSkillPoints = 12;
    expect(players.setBuild(kGuestPlayerId, updatedGuestBuild) == PlayerStateError::None, "guest build can be replaced");
    expect(players.setName(kGuestPlayerId, "Second") == PlayerStateError::None, "guest name can be replaced");
    expect(players.setConnection(kGuestPlayerId, ConnectionState::Disconnected) == PlayerStateError::None, "guest connection state can change");
    expect(players.find(kGuestPlayerId)->build.unspentSkillPoints == 12, "guest build update is retained");
    expect(players.find(kGuestPlayerId)->name == "Second", "guest name update is retained");
    expect(players.find(kGuestPlayerId)->connection == ConnectionState::Disconnected, "guest connection update is retained");
    expect(players.setBuild(PlayerId { 99 }, updatedGuestBuild) == PlayerStateError::PlayerNotFound, "unknown player build cannot be changed");
    expect(players.setName(PlayerId { 99 }, "Missing") == PlayerStateError::PlayerNotFound, "unknown player name cannot be changed");

    expect(registry.rebindObject(guest.entityId, asGameObject(replacementGuest)) == EntityRegistryError::None, "guest object can be replaced beneath player state");
    expect(players.bindingsMatch(registry), "player binding survives object pointer replacement");
    expect(players.find(kGuestPlayerId)->build == updatedGuestBuild, "character build survives object pointer replacement");

    TestObject thirdActor;
    PlayerId thirdPlayerId { 3 };
    EntityRegistrationResult third = registry.registerObject(asGameObject(thirdActor), thirdPlayerId);
    PlayerCharacterState thirdState;
    thirdState.id = thirdPlayerId;
    thirdState.actorId = third.entityId;
    expect(static_cast<bool>(third)
            && players.registerPlayer(thirdState, registry) == PlayerStateError::None,
        "third player state registers");
    expect(players.playerIds() == std::vector<PlayerId> { kHostPlayerId, kGuestPlayerId, thirdPlayerId },
        "player IDs include a third participant in stable order");
    expect(players.unregisterPlayer(thirdPlayerId) == PlayerStateError::None,
        "third player state can be removed");

    expect(players.unregisterPlayer(kGuestPlayerId) == PlayerStateError::None, "guest player state can be removed");
    expect(players.findByActor(guest.entityId) == nullptr, "removed actor binding no longer resolves");
    expect(players.unregisterPlayer(kGuestPlayerId) == PlayerStateError::PlayerNotFound, "player state cannot be removed twice");
    players.clear();
    expect(players.size() == 0, "clearing removes all player states");
}

void testCharacterLobbyValidationAndWireFormat()
{
    CharacterCreationSheet sheet = sampleCharacterSheet(kGuestPlayerId, "Guest");
    expect(validateCharacterSheet(sheet) == CharacterLobbyError::None, "valid level-one character sheet passes validation");

    CharacterCreationSheet invalid = sheet;
    invalid.version++;
    expect(validateCharacterSheet(invalid) == CharacterLobbyError::UnsupportedVersion, "character sheet rejects an unsupported version");
    invalid = sheet;
    invalid.playerId = PlayerId { 99 };
    expect(validateCharacterSheet(invalid) == CharacterLobbyError::InvalidPlayerId, "character sheet rejects an unknown player");
    invalid = sheet;
    invalid.name.clear();
    expect(validateCharacterSheet(invalid) == CharacterLobbyError::EmptyName, "character sheet requires a name");
    invalid = sheet;
    invalid.name.assign(kCharacterNameMaxLength + 1, 'x');
    expect(validateCharacterSheet(invalid) == CharacterLobbyError::NameTooLong, "character sheet bounds name length");
    invalid = sheet;
    invalid.name = "   ";
    expect(validateCharacterSheet(invalid) == CharacterLobbyError::InvalidName, "character sheet rejects a blank display name");
    invalid = sheet;
    invalid.primaryStats[0] = 0;
    expect(validateCharacterSheet(invalid) == CharacterLobbyError::PrimaryStatOutOfRange, "character sheet bounds each SPECIAL stat");
    invalid = sheet;
    invalid.primaryStats[0]++;
    expect(validateCharacterSheet(invalid) == CharacterLobbyError::InvalidPrimaryStatTotal, "character sheet enforces the forty-point SPECIAL budget");
    invalid = sheet;
    invalid.age = 15;
    expect(validateCharacterSheet(invalid) == CharacterLobbyError::AgeOutOfRange, "character sheet rejects an invalid age");
    invalid = sheet;
    invalid.gender = 2;
    expect(validateCharacterSheet(invalid) == CharacterLobbyError::InvalidGender, "character sheet rejects an invalid gender");
    invalid = sheet;
    invalid.taggedSkills[0] = SKILL_COUNT;
    expect(validateCharacterSheet(invalid) == CharacterLobbyError::InvalidTaggedSkill, "character sheet bounds tagged skills");
    invalid = sheet;
    invalid.taggedSkills[1] = invalid.taggedSkills[0];
    expect(validateCharacterSheet(invalid) == CharacterLobbyError::DuplicateTaggedSkill, "character sheet requires distinct tagged skills");
    invalid = sheet;
    invalid.traits[0] = TRAIT_COUNT;
    expect(validateCharacterSheet(invalid) == CharacterLobbyError::InvalidTrait, "character sheet bounds traits");
    invalid = sheet;
    invalid.traits = { TRAIT_GIFTED, TRAIT_GIFTED };
    expect(validateCharacterSheet(invalid) == CharacterLobbyError::DuplicateTrait, "character sheet rejects duplicate traits");
    invalid = sheet;
    invalid.traits = { -1, TRAIT_GIFTED };
    expect(validateCharacterSheet(invalid) == CharacterLobbyError::NonCanonicalTraits, "character sheet requires packed trait slots");

    CharacterBuild build = characterBuildFromSheet(sheet);
    expect(build.baseStats[STAT_LUCK] == 10 && build.baseStats[STAT_AGE] == 25, "validated choices produce a level-one character build");
    expect(build.level == 1 && build.experience == 0 && build.unspentSkillPoints == 0, "new character build starts without progression");
    expect(build.skillPoints[SKILL_SMALL_GUNS] == 0 && build.perkRanks[PERK_AWARENESS] == 0, "new character build cannot inject skill points or perks");
    expect(build.baseStats[STAT_DAMAGE_RESISTANCE_EMP] == 100, "new character build preserves the player EMP resistance default");
    expect(characterSheetFromBuild(sheet.playerId, sheet.name, build) == sheet, "character choices round trip through the canonical build");

    std::vector<std::uint8_t> packet;
    expect(encodeCharacterSheet(sheet, packet) == CharacterLobbyError::None, "valid character sheet encodes");
    expect(packet.size() == kCharacterSheetMinimumPacketSize + sheet.name.size(), "character sheet packet has a bounded fixed-width body");
    CharacterSheetDecodeResult decoded = decodeCharacterSheet(packet);
    expect(static_cast<bool>(decoded) && decoded.sheet == sheet, "character sheet survives its wire round trip");

    std::vector<std::uint8_t> shortPacket(packet.begin(), packet.begin() + 6);
    expect(decodeCharacterSheet(shortPacket).error == CharacterLobbyError::PacketTooShort, "character sheet rejects a short header");
    std::vector<std::uint8_t> wrongVersion = packet;
    wrongVersion[1]++;
    expect(decodeCharacterSheet(wrongVersion).error == CharacterLobbyError::UnsupportedVersion, "character sheet wire format rejects another version");
    std::vector<std::uint8_t> oversizedName = packet;
    oversizedName[6] = static_cast<std::uint8_t>(kCharacterNameMaxLength + 1);
    expect(decodeCharacterSheet(oversizedName).error == CharacterLobbyError::NameTooLong, "character sheet decoder checks name length before copying");
    std::vector<std::uint8_t> truncated = packet;
    truncated.pop_back();
    expect(decodeCharacterSheet(truncated).error == CharacterLobbyError::TruncatedPacket, "character sheet rejects a truncated body");
    std::vector<std::uint8_t> trailing = packet;
    trailing.push_back(0);
    expect(decodeCharacterSheet(trailing).error == CharacterLobbyError::TrailingData, "character sheet rejects trailing data");

    invalid = sheet;
    invalid.primaryStats[0] = 0;
    expect(encodeCharacterSheet(invalid, packet) == CharacterLobbyError::PrimaryStatOutOfRange && packet.empty(), "invalid character sheet leaves no partial packet");
}

void testMultiplayerSaveSidecar()
{
    MultiplayerSaveSidecar sidecar;
    sidecar.generation = 7;
    sidecar.players.resize(2);
    const std::string saveData = "legacy SAVE.DAT bytes";
    sidecar.saveDatDigest = updateMultiplayerSaveDigest(kMultiplayerSaveDigestOffset, saveData.data(), saveData.size());
    sidecar.players[0].playerId = kHostPlayerId;
    sidecar.players[0].name = "Vault Dweller";
    sidecar.players[0].build.baseStats[STAT_STRENGTH] = 8;
    sidecar.players[0].build.skillPoints[SKILL_SMALL_GUNS] = 23;
    sidecar.players[0].build.perkRanks[PERK_AWARENESS] = 1;
    sidecar.players[0].build.taggedSkills = { SKILL_SMALL_GUNS, SKILL_FIRST_AID, SKILL_SPEECH, -1 };
    sidecar.players[0].build.traits = { TRAIT_GIFTED, -1 };
    sidecar.players[0].build.unspentSkillPoints = 4;
    sidecar.players[0].build.level = 6;
    sidecar.players[0].build.experience = 15000;
    sidecar.players[1].playerId = kGuestPlayerId;
    sidecar.players[1].name = "Guest";
    sidecar.players[1].build.baseStats[STAT_AGILITY] = 9;
    sidecar.players[1].build.skillPoints[SKILL_SPEECH] = 17;
    sidecar.players[1].build.taggedSkills = { SKILL_ENERGY_WEAPONS, SKILL_DOCTOR, SKILL_REPAIR, -1 };
    sidecar.players[1].build.level = 5;
    sidecar.players[1].build.experience = 10000;
    sidecar.players[1].objectData = { 0x00, 0x11, 0x00, 0xFE, 0xFF };

    expect(validateMultiplayerSave(sidecar) == MultiplayerSaveError::None, "two progressed character builds form valid save metadata");
    std::vector<std::uint8_t> packet;
    expect(encodeMultiplayerSave(sidecar, packet) == MultiplayerSaveError::None, "multiplayer save metadata encodes");
    expect(packet.size() <= kMultiplayerSaveMaximumSize, "multiplayer sidecar has a strict size bound");
    expect(packet[0] == 'F' && packet[1] == 'C' && packet[2] == 'M' && packet[3] == 'D', "multiplayer sidecar has distinct magic");

    MultiplayerSaveDecodeResult decoded = decodeMultiplayerSave(packet);
    expect(static_cast<bool>(decoded), "multiplayer save metadata decodes");
    expect(decoded.sidecar.generation == 7 && decoded.sidecar.saveDatDigest == sidecar.saveDatDigest, "sidecar keeps generation and SAVE.DAT digest");
    expect(decoded.sidecar.players[0] == sidecar.players[0], "sidecar keeps the host name and full build");
    expect(decoded.sidecar.players[1] == sidecar.players[1], "sidecar keeps the guest name and full build");
    expect(decoded.sidecar.players[1].objectData == sidecar.players[1].objectData, "sidecar keeps opaque recursive guest object data");

    MultiplayerSaveSidecar versionTwo = sidecar;
    versionTwo.version = 2;
    std::vector<std::uint8_t> versionTwoPacket;
    expect(encodeMultiplayerSave(versionTwo, versionTwoPacket) == MultiplayerSaveError::None, "version 2 sidecar still encodes");
    MultiplayerSaveDecodeResult versionTwoDecoded = decodeMultiplayerSave(versionTwoPacket);
    expect(versionTwoDecoded && versionTwoDecoded.sidecar.players == sidecar.players, "version 2 guest inventory migrates into its player record");

    MultiplayerSaveSidecar threePlayer = sidecar;
    SavedPlayerCharacter third = sidecar.players[1];
    third.playerId = PlayerId { 3 };
    third.name = "Third";
    third.objectData = { 0xBE, 0xEF };
    threePlayer.players.push_back(third);
    std::vector<std::uint8_t> threePlayerPacket;
    expect(encodeMultiplayerSave(threePlayer, threePlayerPacket) == MultiplayerSaveError::None, "version 3 stores a bounded third player");
    MultiplayerSaveDecodeResult threePlayerDecoded = decodeMultiplayerSave(threePlayerPacket);
    expect(threePlayerDecoded && threePlayerDecoded.sidecar.players == threePlayer.players, "version 3 restores player-keyed object records without reassigning IDs");
    MultiplayerSaveSidecar replacementRoster = sidecar;
    replacementRoster.players[1].playerId = PlayerId { 3 };
    std::vector<std::uint8_t> replacementPacket;
    expect(encodeMultiplayerSave(replacementRoster, replacementPacket) == MultiplayerSaveError::None,
        "version 3 preserves a missing guest slot instead of renumbering its replacement");
    MultiplayerSaveDecodeResult replacementDecoded = decodeMultiplayerSave(replacementPacket);
    expect(replacementDecoded && replacementDecoded.sidecar.players[1].playerId == PlayerId { 3 },
        "replacement player keeps its explicit ID after decode");

    MultiplayerSaveSidecar legacy = sidecar;
    legacy.version = 1;
    legacy.players[1].objectData.clear();
    std::vector<std::uint8_t> legacyPacket;
    expect(encodeMultiplayerSave(legacy, legacyPacket) == MultiplayerSaveError::None, "version 1 character-only sidecar still encodes");
    MultiplayerSaveDecodeResult legacyDecoded = decodeMultiplayerSave(legacyPacket);
    expect(legacyDecoded && legacyDecoded.sidecar.version == 1 && legacyDecoded.sidecar.players[1].objectData.empty(), "version 1 sidecar remains loadable with an empty guest inventory");

    std::uint64_t changedDigest = updateMultiplayerSaveDigest(kMultiplayerSaveDigestOffset, "legacy SAVE.DAT byteS", saveData.size());
    expect(changedDigest != sidecar.saveDatDigest, "SAVE.DAT digest detects different base-save bytes");

    std::vector<std::uint8_t> badMagic = packet;
    badMagic[0] = 0;
    expect(decodeMultiplayerSave(badMagic).error == MultiplayerSaveError::InvalidMagic, "sidecar rejects invalid magic");
    std::vector<std::uint8_t> badVersion = packet;
    badVersion[4] = 0;
    badVersion[5] = static_cast<std::uint8_t>(kMultiplayerSaveVersion + 1);
    expect(decodeMultiplayerSave(badVersion).error == MultiplayerSaveError::UnsupportedVersion, "sidecar rejects an unsupported version");
    std::vector<std::uint8_t> corrupt = packet;
    corrupt.back() ^= 1;
    expect(decodeMultiplayerSave(corrupt).error == MultiplayerSaveError::ChecksumMismatch, "sidecar rejects corrupt character data");
    std::vector<std::uint8_t> truncated = packet;
    truncated.pop_back();
    expect(decodeMultiplayerSave(truncated).error == MultiplayerSaveError::TruncatedPayload, "sidecar rejects truncated data");
    std::vector<std::uint8_t> trailing = packet;
    trailing.push_back(0);
    expect(decodeMultiplayerSave(trailing).error == MultiplayerSaveError::TrailingData, "sidecar rejects trailing data");

    MultiplayerSaveSidecar invalid = sidecar;
    invalid.generation = 0;
    expect(validateMultiplayerSave(invalid) == MultiplayerSaveError::InvalidGeneration, "sidecar requires a nonzero generation");
    invalid = sidecar;
    std::swap(invalid.players[0], invalid.players[1]);
    expect(validateMultiplayerSave(invalid) == MultiplayerSaveError::NonCanonicalPlayerOrder, "sidecar requires deterministic host-then-guest order");
    invalid = sidecar;
    invalid.players[1].playerId = kHostPlayerId;
    expect(validateMultiplayerSave(invalid) == MultiplayerSaveError::DuplicatePlayer, "sidecar rejects duplicate player slots");
    invalid = threePlayer;
    std::swap(invalid.players[1], invalid.players[2]);
    expect(validateMultiplayerSave(invalid) == MultiplayerSaveError::NonCanonicalPlayerOrder, "sidecar rejects an out-of-order repeated player record");
    invalid = sidecar;
    invalid.players[0].objectData = { 1 };
    expect(validateMultiplayerSave(invalid) == MultiplayerSaveError::InvalidPlayerObjectData, "story actor object data cannot shadow SAVE.DAT");
    invalid = sidecar;
    invalid.players[1].build.taggedSkills[0] = SKILL_COUNT;
    expect(validateMultiplayerSave(invalid) == MultiplayerSaveError::InvalidBuild, "sidecar rejects invalid restored build values");
    invalid = sidecar;
    invalid.version = 1;
    expect(validateMultiplayerSave(invalid) == MultiplayerSaveError::InvalidGuestObjectData, "version 1 sidecar cannot smuggle an unversioned guest object payload");

    TestObject hostActor;
    TestObject guestActor;
    LocalSession source;
    expect(source.start(asGameObject(hostActor), asGameObject(guestActor)) == LocalSessionError::None, "sidecar source session starts");
    expect(source.restorePlayerCharacters(threePlayer) == LocalSessionError::InvalidSaveState, "two-player runtime rejects a sidecar roster it cannot restore completely");
    expect(source.restorePlayerCharacters(replacementRoster) == LocalSessionError::InvalidSaveState, "two-player runtime rejects a missing guest slot");
    expect(source.restorePlayerCharacters(sidecar) == LocalSessionError::None, "validated sidecar restores both registered players");
    expect(source.characterLobbyReady(), "restored players satisfy the loading gate without creation sheets");
    expect(source.transitionTo(SessionPhase::Loading) == LocalSessionError::None, "restored session can enter loading");
    expect(source.players().find(kHostPlayerId)->build == sidecar.players[0].build, "restored host keeps progressed state");
    expect(source.players().find(kGuestPlayerId)->build == sidecar.players[1].build, "restored guest keeps separate progressed state");

    MultiplayerSaveSidecar captured;
    expect(captureMultiplayerSave(source.players(), 8, sidecar.saveDatDigest, captured) == MultiplayerSaveError::None, "active player state can be captured for the next generation");
    expect(captured.generation == 8
            && captured.players[0].playerId == sidecar.players[0].playerId
            && captured.players[0].build == sidecar.players[0].build
            && captured.players[1].playerId == sidecar.players[1].playerId
            && captured.players[1].build == sidecar.players[1].build
            && captured.players[1].objectData.empty(),
        "capture uses canonical player IDs and leaves inventory serialization to the save flow");
}

void testDirectTradeController()
{
    DirectTradeController trade;
    expect(trade.begin(kGuestPlayerId, kHostPlayerId), "trade begins with a canonical bilateral roster");
    expect(trade.state().offers[0].playerId == kHostPlayerId
            && trade.state().offers[1].playerId == kGuestPlayerId,
        "trade participant order does not depend on who invited whom");
    expect(trade.replaceOffer(kHostPlayerId, 1, 12,
               { DirectTradeLine { EntityId { 9 }, 2 }, DirectTradeLine { EntityId { 4 }, 1 } })
            == DirectTradeResult::Applied,
        "host can replace its offer");
    expect(trade.state().revision == 2
            && trade.state().offers[0].items[0].itemId == EntityId { 4 },
        "offer update increments revision and sorts item IDs");
    expect(trade.confirm(kHostPlayerId, 2) == DirectTradeResult::Applied,
        "first confirmation waits for the other player");
    expect(trade.replaceOffer(kGuestPlayerId, 2, 7,
               { DirectTradeLine { EntityId { 21 }, 1 } }) == DirectTradeResult::Applied,
        "guest can change its offer before commit");
    expect(!trade.state().offers[0].confirmed && !trade.state().offers[1].confirmed,
        "an offer change clears both confirmations");
    expect(trade.confirm(kHostPlayerId, 2) == DirectTradeResult::StaleRevision,
        "old confirmation cannot accept a revised offer");
    expect(trade.confirm(kGuestPlayerId, 3) == DirectTradeResult::Applied
            && trade.confirm(kHostPlayerId, 3) == DirectTradeResult::ReadyToCommit,
        "both players can confirm the same revision");
    expect(trade.finishCommit(3, false) && trade.state().revision == 4
            && !trade.state().offers[0].confirmed,
        "failed inventory validation leaves offers in place but requires fresh confirmations");
    expect(trade.replaceOffer(kHostPlayerId, 4, 0,
               { DirectTradeLine { EntityId { 4 }, 1 }, DirectTradeLine { EntityId { 4 }, 2 } })
            == DirectTradeResult::InvalidOffer,
        "duplicate item IDs cannot double-spend one stack");
    expect(trade.state().revision == 4, "invalid offer leaves trade state unchanged");
    expect(trade.confirm(kHostPlayerId, 4) == DirectTradeResult::Applied
            && trade.confirm(kGuestPlayerId, 4) == DirectTradeResult::ReadyToCommit
            && trade.finishCommit(4, true) && !trade.active(),
        "successful commit closes the trade once");
    expect(!trade.finishCommit(4, true), "a committed trade cannot execute twice");
    expect(trade.begin(kHostPlayerId, kGuestPlayerId) && trade.cancel(kGuestPlayerId)
            && !trade.active(),
        "either participant can cancel without moving anything");
    expect(trade.begin(kHostPlayerId, PlayerId { 3 }) && !trade.cancel(kGuestPlayerId),
        "bilateral trade ignores other roster members");
}

void testLootPolicy()
{
    LootPolicy policy;
    expect(policy.reset({ PlayerId { 3 }, kGuestPlayerId, kHostPlayerId }),
        "loot policy accepts a sorted roster independent of join order");
    std::vector<LootCapShare> first = policy.splitCaps(8);
    expect(first.size() == 3 && first[0].playerId == kHostPlayerId
            && first[0].quantity == 3 && first[1].quantity == 3
            && first[2].quantity == 2,
        "cap remainder begins at the first canonical player");
    policy.advanceCaps(8);
    std::vector<LootCapShare> second = policy.splitCaps(4);
    expect(second.size() == 3 && second[0].quantity == 1
            && second[1].quantity == 1 && second[2].quantity == 2,
        "extra caps rotate to the next player across loot pools");
    policy.advanceCaps(4);
    expect(policy.nextExtraCapPlayer() == kHostPlayerId,
        "cap remainder cursor wraps through the roster");
    expect(policy.itemPriority() == kHostPlayerId, "item priority begins at the first player");
    policy.advanceItemPriority();
    expect(policy.itemPriority() == kGuestPlayerId, "contested item priority alternates");
    LootPolicy restored;
    expect(restored.restore({ PlayerId { 3 }, kHostPlayerId, kGuestPlayerId },
               policy.nextExtraCapPlayer(), policy.nextItemPriorityPlayer())
            && restored.splitCaps(4) == policy.splitCaps(4),
        "loot cursors survive roster-based recovery");
    expect(!restored.restore({ kHostPlayerId, kGuestPlayerId }, PlayerId { 3 }, kHostPlayerId),
        "recovery rejects a cursor outside the roster");
}

void testDirectTradeWire()
{
    GameCommand command;
    command.sequence = CommandSequence { 1 };
    command.playerId = kGuestPlayerId;
    command.actorId = EntityId { kGuestPlayerId.value };
    command.expectedPhase = SessionPhase::Exploration;
    command.expectedPhaseRevision = 3;
    command.payload = DirectTradeCommand { DirectTradeAction::Open, kHostPlayerId };
    ProtocolEnvelope envelope = sampleEnvelope();
    expect(encodeGameCommand(command, envelope) == GameplayWireError::None,
        "direct trade open encodes");
    GameCommandDecodeResult decodedCommand = decodeGameCommand(envelope);
    const auto* open = decodedCommand ? std::get_if<DirectTradeCommand>(&decodedCommand.command.payload) : nullptr;
    expect(open != nullptr && open->action == DirectTradeAction::Open
            && open->partnerId == kHostPlayerId,
        "direct trade open retains the bilateral participant ID");
    command.payload = DirectTradeCommand { DirectTradeAction::SetItem, {}, 7,
        EntityId { 42 }, 3 };
    expect(encodeGameCommand(command, envelope) == GameplayWireError::None,
        "revisioned item offer encodes");
    decodedCommand = decodeGameCommand(envelope);
    const auto* item = decodedCommand ? std::get_if<DirectTradeCommand>(&decodedCommand.command.payload) : nullptr;
    expect(item != nullptr && item->revision == 7 && item->itemId == EntityId { 42 }
            && item->quantity == 3,
        "revisioned item offer round trips");
    ProtocolEnvelope malformed = envelope;
    malformed.payload[29] = 1;
    expect(decodeGameCommand(malformed).error == GameplayWireError::InvalidReservedField,
        "direct trade rejects a nonzero reserved byte");
    command.payload = DirectTradeCommand { DirectTradeAction::Confirm, {}, 7 };
    expect(encodeGameCommand(command, envelope) == GameplayWireError::None,
        "trade confirmation encodes");
    command.payload = DirectTradeCommand { DirectTradeAction::SetCaps, {}, 7, {}, 0,
        static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) + 1 };
    expect(encodeGameCommand(command, envelope) == GameplayWireError::InvalidQuantity,
        "trade wire rejects caps outside engine range");

    DirectTradeController trade;
    expect(trade.begin(kHostPlayerId, kGuestPlayerId), "wire fixture trade begins");
    expect(trade.replaceOffer(kGuestPlayerId, 1, 19,
               { DirectTradeLine { EntityId { 42 }, 3 } }) == DirectTradeResult::Applied,
        "wire fixture offer is valid");
    GameEvent event;
    event.sequence = EventSequence { 5 };
    event.causedBy = CommandSequence { 1 };
    event.payload = DirectTradeStateChangedEvent { EntityId { 2 }, trade.state() };
    expect(encodeGameEvent(event, envelope) == GameplayWireError::None,
        "full trade state encodes as one ordered event");
    GameEventDecodeResult decodedEvent = decodeGameEvent(envelope);
    const auto* state = decodedEvent ? std::get_if<DirectTradeStateChangedEvent>(&decodedEvent.event.payload) : nullptr;
    expect(state != nullptr && state->state.revision == 2
            && state->state.offers[1].caps == 19
            && state->state.offers[1].items[0].itemId == EntityId { 42 },
        "trade event preserves revision, caps, and item identity");
    malformed = envelope;
    malformed.payload[26] = 3;
    expect(decodeGameEvent(malformed).error == GameplayWireError::InvalidLength,
        "trade event rejects an oversized participant count");
    event.payload = DirectTradeStateChangedEvent { EntityId { 2 }, {}, true, false };
    expect(encodeGameEvent(event, envelope) == GameplayWireError::None
            && decodeGameEvent(envelope),
        "completed trade event with empty state round trips");
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
    expect(actingPlayerActorOr(asGameObject(guestActor)) == asGameObject(guestActor), "mechanical actor lookup uses its fallback outside a scope");

    {
        ScopedActingPlayerContext hostContext(host, asGameObject(hostActor));
        expect(actingPlayerState() == &host, "acting-player scope exposes its player state");
        expect(actingPlayerActor() == asGameObject(hostActor), "acting-player scope exposes its actor");
        expect(actingPlayerActorOr(asGameObject(guestActor)) == asGameObject(hostActor), "mechanical actor lookup prefers the scoped actor");
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

    expect(bindLocalPlayer(session, kHostPlayerId) == LocalPlayerError::None, "host can become the persistent presentation player");
    {
        ScopedLocalPlayerBinding guestBinding(session, kGuestPlayerId);
        expect(static_cast<bool>(guestBinding), "scoped binding selects the guest");
        expect(localPlayerId() == kGuestPlayerId && localPlayerActor() == asGameObject(guestActor), "scoped binding exposes the guest to modal UI");
    }
    expect(localPlayerId() == kHostPlayerId && localPlayerActor() == asGameObject(hostActor), "leaving a scoped binding restores the host");
    {
        ScopedLocalPlayerBinding guestBinding(asGameObject(guestActor));
        expect(static_cast<bool>(guestBinding), "actor-scoped binding resolves a registered player");
        expect(localPlayerId() == kGuestPlayerId && localPlayerActor() == asGameObject(guestActor), "actor-scoped binding selects the guest that began a deferred action");
    }
    expect(localPlayerId() == kHostPlayerId, "actor-scoped binding restores the host");
    {
        ScopedLocalPlayerBinding invalidBinding(session, PlayerId { 99 });
        expect(!invalidBinding && invalidBinding.error() == LocalPlayerError::PlayerMissing, "scoped binding reports an unknown player");
    }
    expect(localPlayerId() == kHostPlayerId, "failed scoped binding leaves the current player unchanged");

    submitBothCharacterSheets(session);
    expect(playerStateForActor(asGameObject(hostActor)) == session.players().find(kHostPlayerId), "bound session resolves the host state from its actor");
    expect(playerStateForActor(asGameObject(guestActor)) == session.players().find(kGuestPlayerId), "bound session resolves the guest state from its actor");
    expect(session.transitionTo(SessionPhase::Loading) == LocalSessionError::None, "local-player test enters loading");
    expect(session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None, "local-player test enters exploration");
    expect(session.transitionTo(SessionPhase::Transition) == LocalSessionError::None, "local-player test enters map transition");
    expect(bindLocalPlayer(session, kGuestPlayerId) == LocalPlayerError::None, "guest is selected for the actor-rebind test");
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

void testContentManifest()
{
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("fallout-content-manifest-test-"
            + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::filesystem::path patches = root / "data";
    std::error_code filesystemError;
    std::filesystem::create_directories(patches / "SCRIPTS", filesystemError);
    std::filesystem::create_directories(patches / "MAPS", filesystemError);
    std::filesystem::create_directories(patches / "TEXT" / "ENGLISH" / "GAME", filesystemError);
    std::filesystem::create_directories(patches / "SOUND", filesystemError);
    auto writeFile = [](const std::filesystem::path& path, const std::string& contents) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return static_cast<bool>(stream);
    };

    bool fixtureCreated = !filesystemError
        && writeFile(root / "master.dat", "master archive bytes")
        && writeFile(root / "critter.dat", "critter archive bytes")
        && writeFile(patches / "SCRIPTS" / "DOOR.INT", "script version one")
        && writeFile(patches / "MAPS" / "VAULT.MAP", "map version one")
        && writeFile(patches / "TEXT" / "ENGLISH" / "GAME" / "WORLD.MSG", "message version one")
        && writeFile(patches / "SOUND" / "LOCAL.ACM", "presentation-only sound");
    expect(fixtureCreated, "content manifest test fixture is created");
    if (!fixtureCreated) {
        std::filesystem::remove_all(root, filesystemError);
        return;
    }

    ContentManifestResult first = buildContentManifest(
        (root / "master.dat").string(),
        (root / "critter.dat").string(),
        patches.string(),
        patches.string());
    ContentManifestResult identical = buildContentManifest(
        (root / "master.dat").string(),
        (root / "critter.dat").string(),
        patches.string(),
        patches.string());
    expect(first && identical && first.digest == identical.digest && first.digest != 0,
        "content manifest is deterministic for identical archives and patches");

    writeFile(patches / "SOUND" / "LOCAL.ACM", "different presentation sound");
    ContentManifestResult presentationChanged = buildContentManifest(
        (root / "master.dat").string(),
        (root / "critter.dat").string(),
        patches.string(),
        patches.string());
    expect(presentationChanged && presentationChanged.digest == first.digest,
        "content manifest ignores presentation-only patch files");

    writeFile(patches / "SCRIPTS" / "DOOR.INT", "script version two");
    ContentManifestResult scriptChanged = buildContentManifest(
        (root / "master.dat").string(),
        (root / "critter.dat").string(),
        patches.string(),
        patches.string());
    expect(scriptChanged && scriptChanged.digest != first.digest,
        "content manifest detects a patched script change");

    writeFile(root / "master.dat", "changed master archive bytes");
    ContentManifestResult archiveChanged = buildContentManifest(
        (root / "master.dat").string(),
        (root / "critter.dat").string(),
        patches.string(),
        patches.string());
    expect(archiveChanged && archiveChanged.digest != scriptChanged.digest,
        "content manifest hashes every byte of each archive");

    ContentManifestResult missingArchive = buildContentManifest(
        (root / "missing.dat").string(),
        (root / "critter.dat").string(),
        patches.string(),
        patches.string());
    expect(missingArchive.error == ContentManifestError::InvalidPath,
        "content manifest fails closed when an archive is missing");

    std::filesystem::remove_all(root, filesystemError);
}

ProtocolEnvelope gameplayEnvelope(std::uint64_t sequence)
{
    ProtocolEnvelope envelope;
    envelope.sessionId.value = 0xABCDEF0123456789ULL;
    envelope.sequence = sequence;
    return envelope;
}

void testGameplayWireFormat()
{
    std::vector<GameCommand> commands = {
        GameCommand { CommandSequence { 1 }, kGuestPlayerId, EntityId { 20 }, SessionPhase::Exploration, 3, MoveCommand { 12345, 1, true } },
        GameCommand { CommandSequence { 2 }, kGuestPlayerId, EntityId { 20 }, SessionPhase::Exploration, 3, InteractCommand { EntityId { 40 } } },
        GameCommand { CommandSequence { 3 }, kGuestPlayerId, EntityId { 20 }, SessionPhase::Exploration, 3, PickupCommand { EntityId { 41 } } },
        GameCommand { CommandSequence { 4 }, kGuestPlayerId, EntityId { 20 }, SessionPhase::Exploration, 3, LootCommand { EntityId { 42 } } },
        GameCommand { CommandSequence { 5 }, kGuestPlayerId, EntityId { 20 }, SessionPhase::Exploration, 3, FaceCommand { 4 } },
        GameCommand { CommandSequence { 6 }, kGuestPlayerId, EntityId { 20 }, SessionPhase::Exploration, 3, InventoryTransferCommand { EntityId { 42 }, EntityId { 20 }, EntityId { 43 }, 3, 5, ItemDescriptor { 40, 7, 8, 9 } } },
        GameCommand { CommandSequence { 7 }, kGuestPlayerId, EntityId { 20 }, SessionPhase::Exploration, 3, ItemDropCommand { EntityId { 20 }, EntityId { 43 }, 3, 5, ItemDescriptor { 40, 7, 8, 9 } } },
        GameCommand { CommandSequence { 8 }, kGuestPlayerId, EntityId { 20 }, SessionPhase::Exploration, 3, AttackCommand { EntityId { 42 }, 1, 8, 1 } },
        GameCommand { CommandSequence { 9 }, kGuestPlayerId, EntityId { 20 }, SessionPhase::Exploration, 3, SharedModalCommand { SharedModalKind::Dialogue, true } },
        GameCommand { CommandSequence { 10 }, kGuestPlayerId, EntityId { 20 }, SessionPhase::Exploration, 3, UseSkillCommand { EntityId { 40 }, ExplorationSkill::Traps } },
        GameCommand { CommandSequence { 11 }, kGuestPlayerId, EntityId { 20 }, SessionPhase::Exploration, 3, UseItemOnCommand { EntityId { 43 }, EntityId { 40 } } },
        GameCommand { CommandSequence { 12 }, kGuestPlayerId, EntityId { 20 }, SessionPhase::Exploration, 3, ElevatorCommand { 8, 1 } },
        GameCommand { CommandSequence { 13 }, kGuestPlayerId, EntityId { 20 }, SessionPhase::Exploration, 3, ExitGridCommand { EntityId { 46 } } },
        GameCommand { CommandSequence { 14 }, kGuestPlayerId, EntityId { 20 }, SessionPhase::Exploration, 3, SceneryTransitionCommand { EntityId { 47 } } },
        GameCommand { CommandSequence { 15 }, kGuestPlayerId, EntityId { 20 }, SessionPhase::Exploration, 3, RestCommand { 180 } },
    };

    for (std::size_t index = 0; index < commands.size(); index++) {
        ProtocolEnvelope envelope = gameplayEnvelope(index + 10);
        expect(encodeGameCommand(commands[index], envelope) == GameplayWireError::None,
            "gameplay command encodes");
        expect(envelope.kind == MessageKind::Command, "gameplay command sets the command message kind");

        Packet packet;
        expect(encodeEnvelope(envelope, packet) == ProtocolError::None, "gameplay command envelope encodes");
        ProtocolDecodeResult decodedEnvelope = decodeEnvelope(packet);
        GameCommandDecodeResult decoded = decodedEnvelope ? decodeGameCommand(decodedEnvelope.envelope) : GameCommandDecodeResult {};
        expect(static_cast<bool>(decoded), "gameplay command decodes through the protocol envelope");
        if (!decoded) {
            continue;
        }
        expect(decoded.command.sequence == commands[index].sequence
                && decoded.command.playerId == commands[index].playerId
                && decoded.command.actorId == commands[index].actorId
                && decoded.command.expectedPhase == commands[index].expectedPhase
                && decoded.command.expectedPhaseRevision == commands[index].expectedPhaseRevision,
            "gameplay command header round trips");
    }

    ProtocolEnvelope moveEnvelope = gameplayEnvelope(20);
    expect(encodeGameCommand(commands[0], moveEnvelope) == GameplayWireError::None, "movement command encodes for payload checks");
    GameCommandDecodeResult decodedMove = decodeGameCommand(moveEnvelope);
    const MoveCommand* move = decodedMove ? std::get_if<MoveCommand>(&decodedMove.command.payload) : nullptr;
    expect(move != nullptr && move->destinationTile == 12345 && move->elevation == 1 && move->running,
        "movement command payload round trips");

    ProtocolEnvelope doorEnvelope = gameplayEnvelope(21);
    encodeGameCommand(commands[1], doorEnvelope);
    GameCommandDecodeResult decodedDoor = decodeGameCommand(doorEnvelope);
    const InteractCommand* door = decodedDoor ? std::get_if<InteractCommand>(&decodedDoor.command.payload) : nullptr;
    expect(door != nullptr && door->targetId == EntityId { 40 }, "door command target round trips");

    ProtocolEnvelope pickupEnvelope = gameplayEnvelope(22);
    encodeGameCommand(commands[2], pickupEnvelope);
    GameCommandDecodeResult decodedPickup = decodeGameCommand(pickupEnvelope);
    const PickupCommand* pickup = decodedPickup ? std::get_if<PickupCommand>(&decodedPickup.command.payload) : nullptr;
    expect(pickup != nullptr && pickup->targetId == EntityId { 41 }, "pickup command target round trips");

    ProtocolEnvelope lootEnvelope = gameplayEnvelope(23);
    encodeGameCommand(commands[3], lootEnvelope);
    GameCommandDecodeResult decodedLoot = decodeGameCommand(lootEnvelope);
    const LootCommand* loot = decodedLoot ? std::get_if<LootCommand>(&decodedLoot.command.payload) : nullptr;
    expect(loot != nullptr && loot->targetId == EntityId { 42 }, "loot command target round trips");

    ProtocolEnvelope faceEnvelope = gameplayEnvelope(24);
    encodeGameCommand(commands[4], faceEnvelope);
    GameCommandDecodeResult decodedFace = decodeGameCommand(faceEnvelope);
    const FaceCommand* face = decodedFace ? std::get_if<FaceCommand>(&decodedFace.command.payload) : nullptr;
    expect(face != nullptr && face->rotation == 4, "facing command rotation round trips");

    ProtocolEnvelope transferEnvelope = gameplayEnvelope(25);
    encodeGameCommand(commands[5], transferEnvelope);
    GameCommandDecodeResult decodedTransfer = decodeGameCommand(transferEnvelope);
    const InventoryTransferCommand* transfer = decodedTransfer
        ? std::get_if<InventoryTransferCommand>(&decodedTransfer.command.payload)
        : nullptr;
    expect(transfer != nullptr
            && transfer->sourceId == EntityId { 42 }
            && transfer->destinationId == EntityId { 20 }
            && transfer->itemId == EntityId { 43 }
            && transfer->quantity == 3
            && transfer->sourceQuantity == 5
            && transfer->itemDescriptor.data1 == 9,
        "inventory transfer command round trips every authoritative entity and quantity");
    GameCommand dynamicTransfer = commands[5];
    InventoryTransferCommand& dynamicPayload = std::get<InventoryTransferCommand>(dynamicTransfer.payload);
    dynamicPayload.itemId = {};
    ProtocolEnvelope dynamicTransferEnvelope = gameplayEnvelope(26);
    expect(encodeGameCommand(dynamicTransfer, dynamicTransferEnvelope) == GameplayWireError::None,
        "an unregistered item can request host identity from its bounded descriptor");
    GameCommandDecodeResult decodedDynamicTransfer = decodeGameCommand(dynamicTransferEnvelope);
    const InventoryTransferCommand* decodedDynamicPayload = decodedDynamicTransfer
        ? std::get_if<InventoryTransferCommand>(&decodedDynamicTransfer.command.payload)
        : nullptr;
    expect(decodedDynamicPayload != nullptr
            && !isValid(decodedDynamicPayload->itemId)
            && decodedDynamicPayload->sourceQuantity == 5
            && decodedDynamicPayload->itemDescriptor.pid == 40,
        "dynamic item descriptor round trips without a client-assigned entity ID");
    GameCommand invalidTransfer = commands[5];
    std::get<InventoryTransferCommand>(invalidTransfer.payload).quantity = 0;
    ProtocolEnvelope invalidTransferEnvelope = gameplayEnvelope(26);
    expect(encodeGameCommand(invalidTransfer, invalidTransferEnvelope) == GameplayWireError::InvalidQuantity,
        "inventory transfer rejects a zero quantity");
    invalidTransfer = dynamicTransfer;
    std::get<InventoryTransferCommand>(invalidTransfer.payload).itemDescriptor = {};
    expect(encodeGameCommand(invalidTransfer, invalidTransferEnvelope) == GameplayWireError::InvalidEntityId,
        "dynamic inventory transfer requires an item descriptor");

    ProtocolEnvelope dropEnvelope = gameplayEnvelope(27);
    encodeGameCommand(commands[6], dropEnvelope);
    GameCommandDecodeResult decodedDrop = decodeGameCommand(dropEnvelope);
    const ItemDropCommand* drop = decodedDrop
        ? std::get_if<ItemDropCommand>(&decodedDrop.command.payload)
        : nullptr;
    expect(drop != nullptr
            && drop->sourceId == EntityId { 20 }
            && drop->itemId == EntityId { 43 }
            && drop->quantity == 3
            && drop->sourceQuantity == 5
            && drop->itemDescriptor.data0 == 8,
        "item drop command round trips its source stack and descriptor");
    GameCommand invalidDrop = commands[6];
    std::get<ItemDropCommand>(invalidDrop.payload).sourceQuantity = 0;
    expect(encodeGameCommand(invalidDrop, dropEnvelope) == GameplayWireError::InvalidQuantity,
        "item drop rejects an empty source stack");

    ProtocolEnvelope attackEnvelope = gameplayEnvelope(28);
    encodeGameCommand(commands[7], attackEnvelope);
    GameCommandDecodeResult decodedAttack = decodeGameCommand(attackEnvelope);
    const AttackCommand* attack = decodedAttack
        ? std::get_if<AttackCommand>(&decodedAttack.command.payload)
        : nullptr;
    expect(attack != nullptr
            && attack->targetId == EntityId { 42 }
            && attack->hitMode == 1
            && attack->hitLocation == 8,
        "attack command round trips its shared target and combat mode");
    GameCommand invalidAttack = commands[7];
    std::get<AttackCommand>(invalidAttack.payload).hitLocation = 9;
    expect(encodeGameCommand(invalidAttack, attackEnvelope) == GameplayWireError::InvalidAttack,
        "attack command rejects an invalid hit location");

    ProtocolEnvelope modalEnvelope = gameplayEnvelope(29);
    encodeGameCommand(commands[8], modalEnvelope);
    GameCommandDecodeResult decodedModal = decodeGameCommand(modalEnvelope);
    const SharedModalCommand* modal = decodedModal
        ? std::get_if<SharedModalCommand>(&decodedModal.command.payload)
        : nullptr;
    expect(modal != nullptr && modal->kind == SharedModalKind::Dialogue && modal->open,
        "shared modal command round trips its kind and requested state");
    GameCommand invalidModal = commands[8];
    std::get<SharedModalCommand>(invalidModal.payload).kind = static_cast<SharedModalKind>(99);
    expect(encodeGameCommand(invalidModal, modalEnvelope) == GameplayWireError::InvalidModal,
        "shared modal command rejects an unknown kind");
    GameCommand travelProposal = commands[8];
    travelProposal.payload = SharedModalCommand { SharedModalKind::WorldMap, true };
    expect(encodeGameCommand(travelProposal, modalEnvelope) == GameplayWireError::None
            && std::get<SharedModalCommand>(decodeGameCommand(modalEnvelope).command.payload).kind == SharedModalKind::WorldMap,
        "world-map consent uses an authenticated semantic command");
    GameCommand routeCommand = commands[8];
    routeCommand.expectedPhase = SessionPhase::Transition;
    routeCommand.payload = WorldMapRouteCommand { 1399, 1499, false };
    expect(encodeGameCommand(routeCommand, modalEnvelope) == GameplayWireError::None
            && std::get<WorldMapRouteCommand>(decodeGameCommand(modalEnvelope).command.payload).targetY == 1499,
        "world-map route command round trips bounded map coordinates");
    std::get<WorldMapRouteCommand>(routeCommand.payload).targetX = 1400;
    expect(encodeGameCommand(routeCommand, modalEnvelope) == GameplayWireError::InvalidMove,
        "world-map route rejects an out-of-bounds destination");
    routeCommand.payload = WorldMapRouteCommand { -1, -1, true };
    expect(encodeGameCommand(routeCommand, modalEnvelope) == GameplayWireError::None,
        "world-map route can explicitly clear the target");

    ProtocolEnvelope skillEnvelope = gameplayEnvelope(29);
    expect(encodeGameCommand(commands[9], skillEnvelope) == GameplayWireError::None,
        "targeted skill command encodes");
    GameCommandDecodeResult decodedSkill = decodeGameCommand(skillEnvelope);
    const UseSkillCommand* skill = decodedSkill
        ? std::get_if<UseSkillCommand>(&decodedSkill.command.payload)
        : nullptr;
    expect(skill != nullptr
            && skill->targetId == EntityId { 40 }
            && skill->skill == ExplorationSkill::Traps,
        "targeted skill command round trips its target and skill");
    GameCommand invalidSkill = commands[9];
    std::get<UseSkillCommand>(invalidSkill.payload).skill = static_cast<ExplorationSkill>(99);
    expect(encodeGameCommand(invalidSkill, skillEnvelope) == GameplayWireError::InvalidSkill,
        "targeted skill command rejects an unsupported skill");

    ProtocolEnvelope itemUseEnvelope = gameplayEnvelope(30);
    expect(encodeGameCommand(commands[10], itemUseEnvelope) == GameplayWireError::None,
        "item-on-target command encodes");
    GameCommandDecodeResult decodedItemUse = decodeGameCommand(itemUseEnvelope);
    const UseItemOnCommand* itemUse = decodedItemUse
        ? std::get_if<UseItemOnCommand>(&decodedItemUse.command.payload)
        : nullptr;
    expect(itemUse != nullptr
            && itemUse->itemId == EntityId { 43 }
            && itemUse->targetId == EntityId { 40 },
        "item-on-target command round trips both shared identities");
    GameCommand invalidItemUse = commands[10];
    std::get<UseItemOnCommand>(invalidItemUse.payload).targetId = EntityId { 43 };
    expect(encodeGameCommand(invalidItemUse, itemUseEnvelope) == GameplayWireError::InvalidEntityId,
        "item-on-target command rejects an item used on itself");

    ProtocolEnvelope elevatorEnvelope = gameplayEnvelope(31);
    expect(encodeGameCommand(commands[11], elevatorEnvelope) == GameplayWireError::None,
        "elevator command encodes");
    GameCommandDecodeResult decodedElevator = decodeGameCommand(elevatorEnvelope);
    const ElevatorCommand* elevator = decodedElevator
        ? std::get_if<ElevatorCommand>(&decodedElevator.command.payload)
        : nullptr;
    expect(elevator != nullptr && elevator->elevatorType == 8 && elevator->destinationLevel == 1,
        "elevator command round trips its installed-data type and destination level");
    GameCommand invalidElevator = commands[11];
    std::get<ElevatorCommand>(invalidElevator.payload).elevatorType = 12;
    expect(encodeGameCommand(invalidElevator, elevatorEnvelope) == GameplayWireError::InvalidMove,
        "elevator command rejects an unknown elevator table");

    ProtocolEnvelope exitGridEnvelope = gameplayEnvelope(32);
    expect(encodeGameCommand(commands[12], exitGridEnvelope) == GameplayWireError::None,
        "exit-grid command encodes");
    GameCommandDecodeResult decodedExitGrid = decodeGameCommand(exitGridEnvelope);
    const ExitGridCommand* exitGrid = decodedExitGrid
        ? std::get_if<ExitGridCommand>(&decodedExitGrid.command.payload)
        : nullptr;
    expect(exitGrid != nullptr && exitGrid->exitId == EntityId { 46 },
        "exit-grid command round trips its installed-map entity identity");
    GameCommand invalidExitGrid = commands[12];
    std::get<ExitGridCommand>(invalidExitGrid.payload).exitId = {};
    expect(encodeGameCommand(invalidExitGrid, exitGridEnvelope) == GameplayWireError::InvalidEntityId,
        "exit-grid command rejects an invalid source identity");

    ProtocolEnvelope sceneryTransitionEnvelope = gameplayEnvelope(33);
    expect(encodeGameCommand(commands[13], sceneryTransitionEnvelope) == GameplayWireError::None,
        "scenery-transition command encodes");
    GameCommandDecodeResult decodedSceneryTransition = decodeGameCommand(sceneryTransitionEnvelope);
    const SceneryTransitionCommand* sceneryTransition = decodedSceneryTransition
        ? std::get_if<SceneryTransitionCommand>(&decodedSceneryTransition.command.payload)
        : nullptr;
    expect(sceneryTransition != nullptr && sceneryTransition->transitionId == EntityId { 47 },
        "scenery-transition command round trips its registered object identity");

    ProtocolEnvelope restEnvelope = gameplayEnvelope(96);
    expect(encodeGameCommand(commands[14], restEnvelope) == GameplayWireError::None,
        "rest consent command encodes");
    GameCommandDecodeResult decodedRest = decodeGameCommand(restEnvelope);
    const RestCommand* rest = decodedRest
        ? std::get_if<RestCommand>(&decodedRest.command.payload)
        : nullptr;
    expect(rest != nullptr && rest->minutes == 180,
        "rest consent duration round trips");
    GameCommand invalidRest = commands[14];
    std::get<RestCommand>(invalidRest.payload).minutes = 17;
    expect(encodeGameCommand(invalidRest, restEnvelope) == GameplayWireError::InvalidMove,
        "rest command rejects unsupported durations");
    for (std::int32_t choice : { kRestUntilMorning, kRestUntilNoon,
             kRestUntilEvening, kRestUntilMidnight, kRestUntilHealed }) {
        std::get<RestCommand>(invalidRest.payload).minutes = choice;
        expect(encodeGameCommand(invalidRest, restEnvelope) == GameplayWireError::None
                && decodeGameCommand(restEnvelope)
                && std::get<RestCommand>(decodeGameCommand(restEnvelope).command.payload).minutes == choice,
            "until-rest choice round trips without resolving it on the client");
    }
    expect(restMinutesUntilHour(kRestUntilMorning, 600) == 1440
            && restMinutesUntilHour(kRestUntilMorning, 601) == 1439
            && restMinutesUntilHour(kRestUntilMidnight, 2359) == 1
            && restMinutesUntilHour(kRestUntilNoon, 1130) == 30
            && restMinutesUntilHour(kRestUntilHealed, 1200) == 0,
        "until-time choices handle exact hour, rollover, and invalid targets");

    CommandResult accepted;
    accepted.commandSequence.value = 1;
    accepted.status = CommandStatus::Accepted;
    accepted.rejection = CommandRejection::None;
    accepted.firstEventSequence.value = 7;
    accepted.eventCount = 1;
    ProtocolEnvelope acceptedEnvelope = gameplayEnvelope(24);
    expect(encodeCommandResult(accepted, acceptedEnvelope) == GameplayWireError::None,
        "accepted command result encodes");
    CommandResultDecodeResult decodedAccepted = decodeCommandResult(acceptedEnvelope);
    expect(decodedAccepted
            && decodedAccepted.result.commandSequence == accepted.commandSequence
            && decodedAccepted.result.status == accepted.status
            && decodedAccepted.result.rejection == accepted.rejection
            && decodedAccepted.result.firstEventSequence == accepted.firstEventSequence
            && decodedAccepted.result.eventCount == accepted.eventCount,
        "accepted command result round trips");

    CommandResult rejected;
    rejected.commandSequence.value = 2;
    rejected.status = CommandStatus::Rejected;
    rejected.rejection = CommandRejection::NotOwner;
    ProtocolEnvelope rejectedEnvelope = gameplayEnvelope(25);
    expect(encodeCommandResult(rejected, rejectedEnvelope) == GameplayWireError::None,
        "rejected command result encodes");
    CommandResultDecodeResult decodedRejected = decodeCommandResult(rejectedEnvelope);
    expect(decodedRejected && decodedRejected.result.rejection == CommandRejection::NotOwner,
        "rejected command result round trips");

    std::vector<GameEvent> events = {
        GameEvent { EventSequence { 7 }, CommandSequence { 1 }, ActorMovementStartedEvent { EntityId { 20 }, 12345, 1, true, 12340, { 1, 2, 3 } } },
        GameEvent { EventSequence { 8 }, CommandSequence { 2 }, DoorUseStartedEvent { EntityId { 20 }, EntityId { 40 }, true, false, 3 } },
        GameEvent { EventSequence { 9 }, CommandSequence { 3 }, ItemPickupStartedEvent { EntityId { 20 }, EntityId { 41 } } },
        GameEvent { EventSequence { 10 }, CommandSequence { 4 }, LootStartedEvent { EntityId { 20 }, EntityId { 42 } } },
        GameEvent { EventSequence { 11 }, CommandSequence { 5 }, ActorFacingChangedEvent { EntityId { 20 }, 4 } },
        GameEvent { EventSequence { 12 }, CommandSequence { 6 }, InventoryTransferredEvent { EntityId { 20 }, EntityId { 42 }, EntityId { 20 }, EntityId { 43 }, 3, 5, EntityId { 44 }, ItemDescriptor { 40, 7, 8, 9 } } },
        GameEvent { EventSequence { 13 }, CommandSequence { 7 }, ItemDroppedEvent { EntityId { 20 }, EntityId { 20 }, EntityId { 43 }, 3, 5, EntityId { 45 }, 12345, 1, ItemDescriptor { 40, 7, 8, 9 } } },
        GameEvent { EventSequence { 14 }, CommandSequence { 8 }, AttackStartedEvent { EntityId { 20 }, EntityId { 42 }, 1, 8 } },
        GameEvent { EventSequence { 15 }, CommandSequence { 3 }, ItemPickupCompletedEvent { EntityId { 20 }, EntityId { 41 }, true, 1, ItemDescriptor { 40, 7, 8, 9 } } },
        GameEvent { EventSequence { 16 }, CommandSequence { 9 }, SharedModalStateChangedEvent { EntityId { 20 }, SharedModalKind::Dialogue, true, SessionPhase::Dialogue, 4 } },
        GameEvent { EventSequence { 17 }, CommandSequence { 10 }, SkillUseStartedEvent { EntityId { 20 }, EntityId { 40 }, ExplorationSkill::Traps } },
        GameEvent { EventSequence { 18 }, CommandSequence { 11 }, ItemUseStartedEvent { EntityId { 20 }, EntityId { 43 }, EntityId { 40 } } },
        GameEvent { EventSequence { 19 }, CommandSequence { 12 }, ElevatorTransitionedEvent { EntityId { 20 }, 8, 6, 14105, 0, 3, 22504, 1, 2, 5 } },
        GameEvent { EventSequence { 20 }, CommandSequence { 13 }, ExitGridTransitionedEvent {
            EntityId { 20 },
            EntityId { 46 },
            35,
            {
                PlayerTransitionPlacement { kHostPlayerId, EntityId { 10 }, 17091, 0, 0 },
                PlayerTransitionPlacement { kGuestPlayerId, EntityId { 20 }, 17090, 0, 0 },
            },
            7,
        } },
        GameEvent { EventSequence { 21 }, CommandSequence { 14 }, SceneryTransitionedEvent {
            EntityId { 20 },
            EntityId { 47 },
            5,
            {
                PlayerTransitionPlacement { kHostPlayerId, EntityId { 10 }, 15483, 0, 5 },
                PlayerTransitionPlacement { kGuestPlayerId, EntityId { 20 }, 17089, 1, 5 },
            },
            9,
        } },
        GameEvent { EventSequence { 22 }, CommandSequence { 15 }, RestStateChangedEvent { EntityId { 20 }, 180, false, 123456, 9 } },
        GameEvent { EventSequence { 23 }, CommandSequence { 16 }, RestStateChangedEvent { EntityId { 10 }, 180, true, 231456, 11 } },
    };
    for (std::size_t index = 0; index < events.size(); index++) {
        ProtocolEnvelope envelope = gameplayEnvelope(30 + index);
        expect(encodeGameEvent(events[index], envelope) == GameplayWireError::None, "gameplay event encodes");
        GameEventDecodeResult decoded = decodeGameEvent(envelope);
        expect(decoded
                && decoded.event.sequence == events[index].sequence
                && decoded.event.causedBy == events[index].causedBy,
            "gameplay event header round trips");
    }

    ProtocolEnvelope movementEventEnvelope = gameplayEnvelope(40);
    encodeGameEvent(events[0], movementEventEnvelope);
    GameEventDecodeResult decodedMovementEvent = decodeGameEvent(movementEventEnvelope);
    const ActorMovementStartedEvent* movementEvent = decodedMovementEvent
        ? std::get_if<ActorMovementStartedEvent>(&decodedMovementEvent.event.payload)
        : nullptr;
    expect(movementEvent != nullptr
            && movementEvent->actorId == EntityId { 20 }
            && movementEvent->destinationTile == 12345
            && movementEvent->elevation == 1
            && movementEvent->running
            && movementEvent->startingTile == 12340
            && movementEvent->path == std::vector<std::uint8_t>({ 1, 2, 3 }),
        "movement event payload round trips");

    ProtocolEnvelope doorEventEnvelope = gameplayEnvelope(45);
    encodeGameEvent(events[1], doorEventEnvelope);
    GameEventDecodeResult decodedDoorEvent = decodeGameEvent(doorEventEnvelope);
    const DoorUseStartedEvent* doorEvent = decodedDoorEvent
        ? std::get_if<DoorUseStartedEvent>(&decodedDoorEvent.event.payload)
        : nullptr;
    expect(doorEvent != nullptr
            && doorEvent->actorId == EntityId { 20 }
            && doorEvent->targetId == EntityId { 40 }
            && doorEvent->open
            && !doorEvent->locked
            && doorEvent->frame == 3,
        "door event carries authoritative state instead of a script replay request");

    ProtocolEnvelope pickupCompletedEnvelope = gameplayEnvelope(46);
    encodeGameEvent(events[8], pickupCompletedEnvelope);
    GameEventDecodeResult decodedPickupCompleted = decodeGameEvent(pickupCompletedEnvelope);
    const ItemPickupCompletedEvent* pickupCompleted = decodedPickupCompleted
        ? std::get_if<ItemPickupCompletedEvent>(&decodedPickupCompleted.event.payload)
        : nullptr;
    expect(pickupCompleted != nullptr
            && pickupCompleted->actorId == EntityId { 20 }
            && pickupCompleted->targetId == EntityId { 41 }
            && pickupCompleted->succeeded
            && pickupCompleted->quantity == 1
            && pickupCompleted->itemDescriptor.data1 == 9,
        "pickup completion carries the host-selected inventory result");
    GameEvent invalidPickupCompletion = events[8];
    std::get<ItemPickupCompletedEvent>(invalidPickupCompletion.payload).quantity = 0;
    expect(encodeGameEvent(invalidPickupCompletion, pickupCompletedEnvelope) == GameplayWireError::InvalidQuantity,
        "a successful pickup completion requires a concrete quantity");

    ProtocolEnvelope facingEventEnvelope = gameplayEnvelope(41);
    encodeGameEvent(events[4], facingEventEnvelope);
    GameEventDecodeResult decodedFacingEvent = decodeGameEvent(facingEventEnvelope);
    const ActorFacingChangedEvent* facingEvent = decodedFacingEvent
        ? std::get_if<ActorFacingChangedEvent>(&decodedFacingEvent.event.payload)
        : nullptr;
    expect(facingEvent != nullptr
            && facingEvent->actorId == EntityId { 20 }
            && facingEvent->rotation == 4,
        "facing event payload round trips");

    ProtocolEnvelope transferEventEnvelope = gameplayEnvelope(42);
    encodeGameEvent(events[5], transferEventEnvelope);
    GameEventDecodeResult decodedTransferEvent = decodeGameEvent(transferEventEnvelope);
    const InventoryTransferredEvent* transferEvent = decodedTransferEvent
        ? std::get_if<InventoryTransferredEvent>(&decodedTransferEvent.event.payload)
        : nullptr;
    expect(transferEvent != nullptr
            && transferEvent->actorId == EntityId { 20 }
            && transferEvent->sourceId == EntityId { 42 }
            && transferEvent->destinationId == EntityId { 20 }
            && transferEvent->itemId == EntityId { 43 }
            && transferEvent->quantity == 3
            && transferEvent->sourceQuantity == 5
            && transferEvent->remainderItemId == EntityId { 44 }
            && transferEvent->itemDescriptor.extendedFlags == 7,
        "inventory transfer event round trips its accepted mutation");

    ProtocolEnvelope dropEventEnvelope = gameplayEnvelope(43);
    encodeGameEvent(events[6], dropEventEnvelope);
    GameEventDecodeResult decodedDropEvent = decodeGameEvent(dropEventEnvelope);
    const ItemDroppedEvent* dropEvent = decodedDropEvent
        ? std::get_if<ItemDroppedEvent>(&decodedDropEvent.event.payload)
        : nullptr;
    expect(dropEvent != nullptr
            && dropEvent->actorId == EntityId { 20 }
            && dropEvent->sourceId == EntityId { 20 }
            && dropEvent->itemId == EntityId { 43 }
            && dropEvent->quantity == 3
            && dropEvent->sourceQuantity == 5
            && dropEvent->remainderItemId == EntityId { 45 }
            && dropEvent->tile == 12345
            && dropEvent->elevation == 1,
        "item drop event round trips authoritative identity and ground placement");
    GameEvent invalidDropEvent = events[6];
    std::get<ItemDroppedEvent>(invalidDropEvent.payload).remainderItemId = {};
    expect(encodeGameEvent(invalidDropEvent, dropEventEnvelope) == GameplayWireError::InvalidEntityId,
        "partial item drop requires the host-assigned remainder identity");
    GameEvent fullDropEvent = events[6];
    ItemDroppedEvent& fullDropPayload = std::get<ItemDroppedEvent>(fullDropEvent.payload);
    fullDropPayload.quantity = fullDropPayload.sourceQuantity;
    fullDropPayload.remainderItemId = {};
    expect(encodeGameEvent(fullDropEvent, dropEventEnvelope) == GameplayWireError::None,
        "whole-stack item drop does not invent a remainder identity");

    ProtocolEnvelope attackEventEnvelope = gameplayEnvelope(44);
    encodeGameEvent(events[7], attackEventEnvelope);
    GameEventDecodeResult decodedAttackEvent = decodeGameEvent(attackEventEnvelope);
    const AttackStartedEvent* attackEvent = decodedAttackEvent
        ? std::get_if<AttackStartedEvent>(&decodedAttackEvent.event.payload)
        : nullptr;
    expect(attackEvent != nullptr
            && attackEvent->actorId == EntityId { 20 }
            && attackEvent->targetId == EntityId { 42 }
            && attackEvent->hitMode == 1
            && attackEvent->hitLocation == 8,
        "attack event round trips authoritative actor and target IDs");

    ProtocolEnvelope modalEventEnvelope = gameplayEnvelope(47);
    encodeGameEvent(events[9], modalEventEnvelope);
    GameEventDecodeResult decodedModalEvent = decodeGameEvent(modalEventEnvelope);
    const SharedModalStateChangedEvent* modalEvent = decodedModalEvent
        ? std::get_if<SharedModalStateChangedEvent>(&decodedModalEvent.event.payload)
        : nullptr;
    expect(modalEvent != nullptr
            && modalEvent->actorId == EntityId { 20 }
            && modalEvent->kind == SharedModalKind::Dialogue
            && modalEvent->open
            && modalEvent->phase == SessionPhase::Dialogue
            && modalEvent->phaseRevision == 4,
        "shared modal event round trips the authoritative phase boundary");
    GameEvent travelProposalEvent {
        EventSequence { 30 },
        CommandSequence { 9 },
        SharedModalStateChangedEvent { EntityId { 20 }, SharedModalKind::WorldMap, true, SessionPhase::Exploration, 3 },
    };
    expect(encodeGameEvent(travelProposalEvent, modalEventEnvelope) == GameplayWireError::None
            && std::get<SharedModalStateChangedEvent>(decodeGameEvent(modalEventEnvelope).event.payload).phase == SessionPhase::Exploration,
        "world-map proposal event preserves exploration until unanimous consent");
    GameEvent routeEvent {
        EventSequence { 31 },
        CommandSequence { 10 },
        WorldMapRouteSelectedEvent { EntityId { 20 }, 1399, 1499, false },
    };
    expect(encodeGameEvent(routeEvent, modalEventEnvelope) == GameplayWireError::None
            && std::get<WorldMapRouteSelectedEvent>(decodeGameEvent(modalEventEnvelope).event.payload).targetX == 1399,
        "world-map route event round trips the host-approved destination");
    GameEvent arrivalEvent {
        EventSequence { 32 },
        CommandSequence { 10 },
        WorldMapArrivedEvent {
            EntityId { 20 },
            26,
            2,
            {
                PlayerTransitionPlacement { kHostPlayerId, EntityId { 20 }, 20100, 0, 2 },
                PlayerTransitionPlacement { kGuestPlayerId, EntityId { 21 }, 20101, 0, 2 },
            },
            5, 725, 616, 123456, WorldMapArrivalKind::Encounter,
        },
    };
    ProtocolEnvelope arrivalEnvelope = gameplayEnvelope(49);
    GameEventDecodeResult decodedArrival;
    expect(encodeGameEvent(arrivalEvent, arrivalEnvelope) == GameplayWireError::None
            && (decodedArrival = decodeGameEvent(arrivalEnvelope)),
        "world-map arrival event encodes and decodes");
    const auto* arrival = std::get_if<WorldMapArrivedEvent>(&decodedArrival.event.payload);
    expect(arrival != nullptr && arrival->map == 26
            && arrival->entranceIndex == 2
            && arrival->worldX == 725 && arrival->worldY == 616
            && arrival->gameTime == 123456
            && arrival->kind == WorldMapArrivalKind::Encounter
            && arrival->placements.size() == 2
            && arrival->placements[1].actorId == EntityId { 21 },
        "world-map arrival carries the map, clock, and player-keyed placements");
    GameEvent duplicateArrival = arrivalEvent;
    std::get<WorldMapArrivedEvent>(duplicateArrival.payload).placements[1].playerId = kHostPlayerId;
    expect(encodeGameEvent(duplicateArrival, arrivalEnvelope) == GameplayWireError::InvalidMove,
        "world-map arrival rejects duplicate player placements");
    GameEvent fatalArrival = arrivalEvent;
    std::get<WorldMapArrivedEvent>(fatalArrival.payload).kind = WorldMapArrivalKind::Fatal;
    std::get<WorldMapArrivedEvent>(fatalArrival.payload).entranceIndex = 0;
    expect(encodeGameEvent(fatalArrival, arrivalEnvelope) == GameplayWireError::None
            && std::get<WorldMapArrivedEvent>(decodeGameEvent(arrivalEnvelope).event.payload).kind
                == WorldMapArrivalKind::Fatal,
        "fatal world-map event survives the arrival wire format");
    std::get<WorldMapArrivedEvent>(fatalArrival.payload).entranceIndex = 33;
    expect(encodeGameEvent(fatalArrival, arrivalEnvelope) == GameplayWireError::InvalidMove,
        "world-map arrival rejects an invalid entrance index");

    ProtocolEnvelope elevatorEventEnvelope = gameplayEnvelope(48);
    encodeGameEvent(events[12], elevatorEventEnvelope);
    GameEventDecodeResult decodedElevatorEvent = decodeGameEvent(elevatorEventEnvelope);
    const ElevatorTransitionedEvent* elevatorEvent = decodedElevatorEvent
        ? std::get_if<ElevatorTransitionedEvent>(&decodedElevatorEvent.event.payload)
        : nullptr;
    expect(elevatorEvent != nullptr
            && elevatorEvent->actorId == EntityId { 20 }
            && elevatorEvent->elevatorType == 8
            && elevatorEvent->map == 6
            && elevatorEvent->hostTile == 14105
            && elevatorEvent->hostElevation == 0
            && elevatorEvent->hostRotation == 3
            && elevatorEvent->guestTile == 22504
            && elevatorEvent->guestElevation == 1
            && elevatorEvent->guestRotation == 2
            && elevatorEvent->phaseRevision == 5,
        "elevator event round trips each actor's independent authoritative placement state");

    ProtocolEnvelope exitGridEventEnvelope = gameplayEnvelope(50);
    expect(encodeGameEvent(events[13], exitGridEventEnvelope) == GameplayWireError::None,
        "exit-grid event encodes its bounded placement roster");
    GameEventDecodeResult decodedExitGridEvent = decodeGameEvent(exitGridEventEnvelope);
    const ExitGridTransitionedEvent* exitGridEvent = decodedExitGridEvent
        ? std::get_if<ExitGridTransitionedEvent>(&decodedExitGridEvent.event.payload)
        : nullptr;
    expect(exitGridEvent != nullptr
            && exitGridEvent->actorId == EntityId { 20 }
            && exitGridEvent->exitId == EntityId { 46 }
            && exitGridEvent->map == 35
            && exitGridEvent->phaseRevision == 7
            && exitGridEvent->placements.size() == 2
            && exitGridEvent->placements[0].playerId == kHostPlayerId
            && exitGridEvent->placements[0].actorId == EntityId { 10 }
            && exitGridEvent->placements[1].playerId == kGuestPlayerId
            && exitGridEvent->placements[1].tile == 17090,
        "exit-grid event round trips player-keyed destination placements");
    GameEvent duplicateExitPlacement = events[13];
    std::get<ExitGridTransitionedEvent>(duplicateExitPlacement.payload).placements[1].playerId = kHostPlayerId;
    expect(encodeGameEvent(duplicateExitPlacement, exitGridEventEnvelope) == GameplayWireError::InvalidMove,
        "exit-grid event rejects duplicate roster participants");

    ProtocolEnvelope sceneryTransitionEventEnvelope = gameplayEnvelope(51);
    expect(encodeGameEvent(events[14], sceneryTransitionEventEnvelope) == GameplayWireError::None,
        "scenery-transition event encodes its bounded placement roster");
    GameEventDecodeResult decodedSceneryTransitionEvent = decodeGameEvent(sceneryTransitionEventEnvelope);
    const SceneryTransitionedEvent* sceneryTransitionEvent = decodedSceneryTransitionEvent
        ? std::get_if<SceneryTransitionedEvent>(&decodedSceneryTransitionEvent.event.payload)
        : nullptr;
    expect(sceneryTransitionEvent != nullptr
            && sceneryTransitionEvent->actorId == EntityId { 20 }
            && sceneryTransitionEvent->transitionId == EntityId { 47 }
            && sceneryTransitionEvent->map == 5
            && sceneryTransitionEvent->phaseRevision == 9
            && sceneryTransitionEvent->placements.size() == 2
            && sceneryTransitionEvent->placements[1].elevation == 1,
        "scenery-transition event round trips player-keyed independent placements");

    ProtocolEnvelope restEventEnvelope = gameplayEnvelope(97);
    expect(encodeGameEvent(events[16], restEventEnvelope) == GameplayWireError::None,
        "completed rest event encodes");
    GameEventDecodeResult decodedRestEvent = decodeGameEvent(restEventEnvelope);
    const RestStateChangedEvent* completedRest = decodedRestEvent
        ? std::get_if<RestStateChangedEvent>(&decodedRestEvent.event.payload)
        : nullptr;
    expect(completedRest != nullptr
            && completedRest->actorId == EntityId { 10 }
            && completedRest->minutes == 180
            && completedRest->completed
            && completedRest->gameTime == 231456
            && completedRest->phaseRevision == 11,
        "completed rest event round trips final world time and phase");
    GameEvent interruptedRest = events[16];
    auto& interruptedPayload = std::get<RestStateChangedEvent>(interruptedRest.payload);
    interruptedPayload.minutes = kRestUntilHealed;
    interruptedPayload.interrupted = true;
    expect(encodeGameEvent(interruptedRest, restEventEnvelope) == GameplayWireError::None
            && decodeGameEvent(restEventEnvelope)
            && std::get<RestStateChangedEvent>(decodeGameEvent(restEventEnvelope).event.payload).interrupted,
        "interrupted until-healed completion round trips its stop reason");
    interruptedPayload.completed = false;
    expect(encodeGameEvent(interruptedRest, restEventEnvelope) == GameplayWireError::InvalidMove,
        "rest proposal cannot claim an interruption before rest begins");

    ProtocolEnvelope skillEventEnvelope = gameplayEnvelope(48);
    encodeGameEvent(events[10], skillEventEnvelope);
    GameEventDecodeResult decodedSkillEvent = decodeGameEvent(skillEventEnvelope);
    const SkillUseStartedEvent* skillEvent = decodedSkillEvent
        ? std::get_if<SkillUseStartedEvent>(&decodedSkillEvent.event.payload)
        : nullptr;
    expect(skillEvent != nullptr
            && skillEvent->actorId == EntityId { 20 }
            && skillEvent->targetId == EntityId { 40 }
            && skillEvent->skill == ExplorationSkill::Traps,
        "skill-use event round trips without requesting replica-side rules");

    ProtocolEnvelope itemUseEventEnvelope = gameplayEnvelope(49);
    encodeGameEvent(events[11], itemUseEventEnvelope);
    GameEventDecodeResult decodedItemUseEvent = decodeGameEvent(itemUseEventEnvelope);
    const ItemUseStartedEvent* itemUseEvent = decodedItemUseEvent
        ? std::get_if<ItemUseStartedEvent>(&decodedItemUseEvent.event.payload)
        : nullptr;
    expect(itemUseEvent != nullptr
            && itemUseEvent->actorId == EntityId { 20 }
            && itemUseEvent->itemId == EntityId { 43 }
            && itemUseEvent->targetId == EntityId { 40 },
        "item-use event round trips as a presentation cue without replica-side scripts");

    ProtocolEnvelope missingSession = gameplayEnvelope(42);
    missingSession.sessionId = {};
    expect(encodeGameCommand(commands[0], missingSession) == GameplayWireError::InvalidSessionId,
        "gameplay encoder rejects a missing session ID");

    ProtocolEnvelope malformed = moveEnvelope;
    malformed.payload[3] = 1;
    expect(decodeGameCommand(malformed).error == GameplayWireError::InvalidReservedField,
        "command decoder rejects a reserved header byte");
    malformed = moveEnvelope;
    malformed.payload[2] = 99;
    expect(decodeGameCommand(malformed).error == GameplayWireError::UnknownPayloadType,
        "command decoder rejects an unknown payload type");
    malformed = moveEnvelope;
    malformed.payload[36] = 2;
    expect(decodeGameCommand(malformed).error == GameplayWireError::InvalidReservedField,
        "command decoder rejects a noncanonical boolean");
    malformed = moveEnvelope;
    malformed.payload.push_back(0);
    expect(decodeGameCommand(malformed).error == GameplayWireError::InvalidLength,
        "command decoder rejects trailing bytes");

    CommandResult inconsistent = accepted;
    inconsistent.eventCount = 0;
    ProtocolEnvelope invalidResultEnvelope = gameplayEnvelope(42);
    expect(encodeCommandResult(inconsistent, invalidResultEnvelope) == GameplayWireError::InconsistentResult,
        "accepted result requires its declared event range");
    inconsistent = rejected;
    inconsistent.rejection = static_cast<CommandRejection>(99);
    expect(encodeCommandResult(inconsistent, invalidResultEnvelope) == GameplayWireError::InvalidRejection,
        "rejected result requires a known rejection reason");

    ProtocolEnvelope malformedEvent = movementEventEnvelope;
    malformedEvent.payload[37] = 1;
    expect(decodeGameEvent(malformedEvent).error == GameplayWireError::InvalidReservedField,
        "event decoder rejects a reserved movement byte");
    malformedEvent = movementEventEnvelope;
    malformedEvent.payload.back() = kActorRotationCount;
    expect(decodeGameEvent(malformedEvent).error == GameplayWireError::InvalidMove,
        "event decoder rejects an invalid path direction");
    malformedEvent = movementEventEnvelope;
    malformedEvent.payload.pop_back();
    expect(decodeGameEvent(malformedEvent).error == GameplayWireError::InvalidLength,
        "event decoder rejects a truncated movement path");
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

    Packet oversized(kMaxTransportPacketSize + 1);
    expect(pair.first->send(std::move(oversized)) == TransportSendResult::PacketTooLarge, "loopback rejects an oversized packet before queueing it");
    pair.second->close();
    expect(!pair.first->isConnected(), "closing one endpoint disconnects its peer");
    expect(pair.first->send(Packet { 7 }) == TransportSendResult::Disconnected, "send fails after peer closes");
}

std::unique_ptr<Transport> acceptTcpPeer(TcpListener& listener)
{
    for (int attempt = 0; attempt < 10000; attempt++) {
        std::unique_ptr<Transport> transport = listener.accept();
        if (transport != nullptr) {
            return transport;
        }
    }
    return nullptr;
}

std::optional<Packet> receiveTcpPacket(Transport& transport, Transport* peer = nullptr)
{
    for (int attempt = 0; attempt < 10000; attempt++) {
        if (peer != nullptr) {
            peer->poll();
        }
        std::optional<Packet> packet = transport.receive();
        if (packet.has_value()) {
            return packet;
        }
    }
    return std::nullopt;
}

void testTcpTransportAndHandshake()
{
    TcpListenResult listening = listenTcp(0);
    expect(static_cast<bool>(listening), "TCP host listens on an available port");
    if (!listening) {
        return;
    }
    expect(listening.listener->port() != 0 && listening.listener->isOpen(), "TCP host reports its assigned port");

    TcpConnectResult joining = connectTcp("127.0.0.1", listening.listener->port());
    expect(static_cast<bool>(joining), "TCP guest connects by direct IP");
    if (!joining) {
        return;
    }

    std::unique_ptr<Transport> host = acceptTcpPeer(*listening.listener);
    expect(host != nullptr && host->isConnected() && joining.transport->isConnected(), "TCP host accepts the guest connection");
    if (host == nullptr) {
        return;
    }

    constexpr std::uint64_t contentDigest = 0x1020304050607080ULL;
    SessionId sessionId { 0x8877665544332211ULL };
    ReconnectToken reconnectToken;
    for (std::size_t index = 0; index < reconnectToken.bytes.size(); index++) {
        reconnectToken.bytes[index] = static_cast<std::uint8_t>(index + 1);
    }
    ProtocolEnvelope helloEnvelope;
    helloEnvelope.sequence = 1;
    expect(encodeHandshakeMessage(HandshakeHello { contentDigest }, helloEnvelope) == HandshakeError::None, "guest handshake hello encodes");

    Packet helloPacket;
    expect(encodeEnvelope(helloEnvelope, helloPacket) == ProtocolError::None, "guest handshake envelope encodes");
    expect(joining.transport->send(std::move(helloPacket)) == TransportSendResult::Sent, "guest sends its handshake");

    std::optional<Packet> receivedHello = receiveTcpPacket(*host, joining.transport.get());
    expect(receivedHello.has_value(), "host receives one framed handshake packet");
    if (!receivedHello.has_value()) {
        return;
    }
    ProtocolDecodeResult decodedHelloEnvelope = decodeEnvelope(*receivedHello);
    HandshakeDecodeResult decodedHello = decodedHelloEnvelope ? decodeHandshakeMessage(decodedHelloEnvelope.envelope) : HandshakeDecodeResult {};
    const HandshakeHello* hello = decodedHello ? std::get_if<HandshakeHello>(&decodedHello.message) : nullptr;
    expect(hello != nullptr && hello->contentDigest == contentDigest, "host decodes the guest content digest");
    if (hello == nullptr) {
        return;
    }

    HandshakeMessage response = makeHostHandshakeResponse(*hello, contentDigest, sessionId, reconnectToken);
    ProtocolEnvelope welcomeEnvelope;
    welcomeEnvelope.sequence = 1;
    expect(encodeHandshakeMessage(response, welcomeEnvelope) == HandshakeError::None, "host handshake welcome encodes");
    Packet welcomePacket;
    expect(encodeEnvelope(welcomeEnvelope, welcomePacket) == ProtocolError::None, "host handshake envelope encodes");
    expect(host->send(std::move(welcomePacket)) == TransportSendResult::Sent, "host sends its handshake response");

    std::optional<Packet> receivedWelcome = receiveTcpPacket(*joining.transport, host.get());
    expect(receivedWelcome.has_value(), "guest receives one framed welcome packet");
    if (receivedWelcome.has_value()) {
        ProtocolDecodeResult decodedWelcomeEnvelope = decodeEnvelope(*receivedWelcome);
        HandshakeDecodeResult decodedWelcome = decodedWelcomeEnvelope ? decodeHandshakeMessage(decodedWelcomeEnvelope.envelope) : HandshakeDecodeResult {};
        const HandshakeWelcome* welcome = decodedWelcome ? std::get_if<HandshakeWelcome>(&decodedWelcome.message) : nullptr;
        expect(welcome != nullptr && welcome->sessionId == sessionId
                && welcome->assignedPlayerId == kGuestPlayerId
                && reconnectTokensEqual(welcome->reconnectToken, reconnectToken),
            "guest accepts its session, player assignment, and reconnect credential");
    }
    std::optional<TransportPeerIdentity> pinnedIdentity = joining.transport->peerIdentity();
    expect(pinnedIdentity.has_value(), "TLS guest records the host certificate identity");

    expect(host->send(Packet { 1, 2, 3 }) == TransportSendResult::Sent, "TCP transport queues the first gameplay packet");
    expect(host->send(Packet { 4, 5 }) == TransportSendResult::Sent, "TCP transport queues a second gameplay packet");
    std::optional<Packet> first = receiveTcpPacket(*joining.transport, host.get());
    std::optional<Packet> second = receiveTcpPacket(*joining.transport, host.get());
    expect(first == Packet({ 1, 2, 3 }) && second == Packet({ 4, 5 }), "TCP framing preserves packet boundaries and order");

    Packet multiRecordPacket(128 * 1024);
    for (std::size_t index = 0; index < multiRecordPacket.size(); index++) {
        multiRecordPacket[index] = static_cast<std::uint8_t>(index & 0xFF);
    }
    Packet expectedMultiRecordPacket = multiRecordPacket;
    expect(host->send(std::move(multiRecordPacket)) == TransportSendResult::Sent
            && receiveTcpPacket(*joining.transport, host.get()) == expectedMultiRecordPacket,
        "TLS framing preserves a packet split across multiple encrypted records");

    Packet oversized(kMaxTransportPacketSize + 1);
    expect(host->send(std::move(oversized)) == TransportSendResult::PacketTooLarge, "TCP transport rejects oversized packets without disconnecting");
    expect(host->isConnected(), "oversized packet rejection leaves the TCP connection open");

    host->close();
    joining.transport->close();
    TcpConnectResult reconnecting = connectTcp("127.0.0.1", listening.listener->port(), 5000, pinnedIdentity);
    std::unique_ptr<Transport> reconnectedHost = acceptTcpPeer(*listening.listener);
    expect(reconnecting && reconnectedHost != nullptr, "TLS guest reconnects to the pinned host identity");
    if (reconnecting && reconnectedHost != nullptr) {
        expect(reconnecting.transport->send(Packet { 8, 9 }) == TransportSendResult::Sent,
            "pinned TLS reconnect queues application data");
        expect(receiveTcpPacket(*reconnectedHost, reconnecting.transport.get()) == Packet({ 8, 9 }),
            "pinned TLS reconnect carries application data");
        reconnecting.transport->close();
        reconnectedHost->close();
    }

    if (pinnedIdentity.has_value()) {
        TransportPeerIdentity wrongIdentity = *pinnedIdentity;
        wrongIdentity.back() ^= 1;
        TcpConnectResult impostor = connectTcp("127.0.0.1", listening.listener->port(), 5000, wrongIdentity);
        std::unique_ptr<Transport> impostorHost = acceptTcpPeer(*listening.listener);
        expect(impostor && impostorHost != nullptr, "mismatched-pin test establishes its TCP socket");
        if (impostor && impostorHost != nullptr) {
            expect(impostor.transport->send(Packet { 0xA5 }) == TransportSendResult::Sent,
                "application bytes can queue while the TLS pin is checked");
            for (int attempt = 0; attempt < 10000 && impostor.transport->isConnected(); attempt++) {
                impostorHost->poll();
                impostor.transport->poll();
            }
            expect(!impostor.transport->isConnected() && !impostorHost->receive().has_value(),
                "TLS reconnect rejects a different host identity before releasing queued credentials");
            impostorHost->close();
        }
    }

    HandshakeMessage mismatch = makeHostHandshakeResponse(HandshakeHello { contentDigest + 1 }, contentDigest, sessionId, reconnectToken);
    const HandshakeRejected* mismatchRejection = std::get_if<HandshakeRejected>(&mismatch);
    expect(mismatchRejection != nullptr && mismatchRejection->reason == HandshakeRejection::ContentMismatch, "host rejects a different content digest");
    HandshakeMessage full = makeHostHandshakeResponse(HandshakeHello { contentDigest }, contentDigest, sessionId, reconnectToken, false);
    const HandshakeRejected* fullRejection = std::get_if<HandshakeRejected>(&full);
    expect(fullRejection != nullptr && fullRejection->reason == HandshakeRejection::SessionFull, "host rejects a third player");

    ProtocolEnvelope malformed = helloEnvelope;
    malformed.payload.push_back(0);
    expect(decodeHandshakeMessage(malformed).error == HandshakeError::InvalidLength, "handshake rejects trailing payload bytes");
    malformed = helloEnvelope;
    malformed.payload[3] = 1;
    expect(decodeHandshakeMessage(malformed).error == HandshakeError::InvalidReservedField, "handshake rejects nonzero reserved fields");

    expect(joining.transport->send(Packet { 9 }) == TransportSendResult::Disconnected, "closed TCP endpoint rejects sends");
    listening.listener->close();
    expect(!listening.listener->isOpen(), "TCP listener closes explicitly");

    expect(connectTcp("", 0).error == TcpError::InvalidArgument, "TCP join rejects an empty address and port");
    expect(connectTcp("127.0.0.1", 1, 0).error == TcpError::InvalidArgument, "TCP join rejects a zero timeout");
}

NetworkLaunchParseResult parseLaunchArguments(std::initializer_list<const char*> arguments)
{
    std::vector<std::string> storage(arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (std::string& argument : storage) {
        argv.push_back(argument.data());
    }
    return parseNetworkLaunchOptions(static_cast<int>(argv.size()), argv.data());
}

void pollBootstraps(NetworkBootstrap& host, NetworkBootstrap& guest)
{
    for (int attempt = 0; attempt < 10000; attempt++) {
        host.poll();
        guest.poll();
        bool hostDone = host.state() == NetworkBootstrapState::Connected
            || host.state() == NetworkBootstrapState::Rejected
            || host.state() == NetworkBootstrapState::Failed;
        bool guestDone = guest.state() == NetworkBootstrapState::Connected
            || guest.state() == NetworkBootstrapState::Rejected
            || guest.state() == NetworkBootstrapState::Failed;
        if (hostDone && guestDone) {
            return;
        }
    }
}

void testNetworkLaunchAndBootstrap()
{
    NetworkLaunchParseResult disabled = parseLaunchArguments({ "fallout-ce" });
    expect(disabled && disabled.options.mode == NetworkLaunchMode::Disabled, "network launch defaults to single-player mode");

    NetworkLaunchParseResult hostDefault = parseLaunchArguments({ "fallout-ce", "--multiplayer-host" });
    expect(hostDefault && hostDefault.options.mode == NetworkLaunchMode::Host && hostDefault.options.port == kDefaultMultiplayerPort,
        "host launch uses the default multiplayer port");

    NetworkLaunchParseResult hostPort = parseLaunchArguments({ "fallout-ce", "--multiplayer-host=45123" });
    expect(hostPort && hostPort.options.mode == NetworkLaunchMode::Host && hostPort.options.port == 45123,
        "host launch accepts an explicit port");

    NetworkLaunchParseResult joinDefault = parseLaunchArguments({ "fallout-ce", "--multiplayer-join", "example.test" });
    expect(joinDefault && joinDefault.options.mode == NetworkLaunchMode::Join
            && joinDefault.options.address == "example.test" && joinDefault.options.port == kDefaultMultiplayerPort,
        "join launch accepts a host name and default port");

    NetworkLaunchParseResult joinPort = parseLaunchArguments({ "fallout-ce", "--multiplayer-join=127.0.0.1:45124" });
    expect(joinPort && joinPort.options.address == "127.0.0.1" && joinPort.options.port == 45124,
        "join launch accepts a direct IPv4 address and explicit port");

    NetworkLaunchParseResult joinIpv6 = parseLaunchArguments({ "fallout-ce", "--multiplayer-join=[::1]:45125" });
    expect(joinIpv6 && joinIpv6.options.address == "::1" && joinIpv6.options.port == 45125,
        "join launch parses a bracketed IPv6 endpoint");

    TransportPeerIdentity expectedIdentity;
    for (std::size_t index = 0; index < expectedIdentity.size(); index++) {
        expectedIdentity[index] = static_cast<std::uint8_t>(index);
    }
    std::string formattedIdentity = formatTransportPeerIdentity(expectedIdentity);
    TransportPeerIdentity parsedIdentity;
    expect(formattedIdentity.size() == expectedIdentity.size() * 2
            && parseTransportPeerIdentity(formattedIdentity, parsedIdentity)
            && parsedIdentity == expectedIdentity,
        "host certificate fingerprints format and parse without losing bytes");
    std::string fingerprintArgument = "--multiplayer-host-fingerprint=" + formattedIdentity;
    NetworkLaunchParseResult pinnedJoin = parseLaunchArguments({
        "fallout-ce",
        "--multiplayer-join=127.0.0.1:45124",
        fingerprintArgument.c_str(),
    });
    expect(pinnedJoin && pinnedJoin.options.expectedHostIdentity == expectedIdentity,
        "join launch accepts an explicit host certificate fingerprint");

    std::string address;
    std::uint16_t port = 0;
    expect(parseNetworkJoinEndpoint("localhost", address, port)
            && address == "localhost" && port == kDefaultMultiplayerPort,
        "lobby join parser accepts a host with the default port");
    expect(parseNetworkJoinEndpoint("127.0.0.1:45126", address, port)
            && address == "127.0.0.1" && port == 45126,
        "lobby join parser accepts an IPv4 endpoint");
    expect(!parseNetworkJoinEndpoint("localhost:0", address, port),
        "lobby join parser rejects an invalid port");

    expect(parseLaunchArguments({ "fallout-ce", "--multiplayer-join" }).error == NetworkLaunchParseError::MissingJoinAddress,
        "join launch rejects a missing address");
    expect(parseLaunchArguments({ "fallout-ce", "--multiplayer-join", "--some-other-option" }).error == NetworkLaunchParseError::MissingJoinAddress,
        "join launch does not consume another option as its address");
    expect(parseLaunchArguments({ "fallout-ce", "--multiplayer-host=0" }).error == NetworkLaunchParseError::InvalidPort,
        "host launch rejects port zero");
    expect(parseLaunchArguments({ "fallout-ce", "--multiplayer-join=localhost:70000" }).error == NetworkLaunchParseError::InvalidPort,
        "join launch rejects an out-of-range port");
    expect(parseLaunchArguments({ "fallout-ce", "--multiplayer-host", "--multiplayer-join=localhost" }).error == NetworkLaunchParseError::ConflictingModes,
        "network launch rejects simultaneous host and join modes");
    expect(parseLaunchArguments({ "fallout-ce", "--multiplayer-host", "--multiplayer-dev" }).error == NetworkLaunchParseError::DevelopmentModeConflict,
        "network launch rejects simultaneous TCP and developer modes");
    expect(parseLaunchArguments({ "fallout-ce", "--multiplayer-join=localhost", "--multiplayer-host-fingerprint=1234" }).error == NetworkLaunchParseError::InvalidHostIdentity,
        "join launch rejects an invalid host certificate fingerprint");
    expect(parseLaunchArguments({ "fallout-ce", "--multiplayer-host", fingerprintArgument.c_str() }).error == NetworkLaunchParseError::HostIdentityWithoutJoin,
        "host launch rejects a guest-only certificate fingerprint");
    expect(parseLaunchArguments({ "fallout-ce", "--multiplayer-join=localhost", fingerprintArgument.c_str(),
               "--multiplayer-host-fingerprint", formattedIdentity.c_str() })
            .error
            == NetworkLaunchParseError::DuplicateHostIdentity,
        "join launch rejects duplicate host certificate fingerprints");

    constexpr std::uint64_t contentDigest = 0xA1B2C3D4E5F60718ULL;
    SessionId sessionId { 0x123456789ABCDEF0ULL };
    NetworkLaunchOptions hostOptions;
    hostOptions.mode = NetworkLaunchMode::Host;
    hostOptions.port = 0;
    NetworkBootstrap host;
    expect(host.start(hostOptions, contentDigest, sessionId), "network host bootstrap listens on an available test port");
    expect(host.localIdentity().has_value(), "network host exposes its certificate fingerprint before accepting a guest");

    NetworkLaunchOptions guestOptions;
    guestOptions.mode = NetworkLaunchMode::Join;
    guestOptions.address = "127.0.0.1";
    guestOptions.port = host.port();
    guestOptions.expectedHostIdentity = host.localIdentity();
    NetworkBootstrap guest;
    expect(guest.start(guestOptions, contentDigest), "network guest bootstrap connects and sends hello");
    pollBootstraps(host, guest);
    expect(host.state() == NetworkBootstrapState::Connected && guest.state() == NetworkBootstrapState::Connected,
        "host and guest bootstraps complete the connection handshake");
    expect(host.sessionId() == sessionId && guest.sessionId() == sessionId,
        "host assigns its authoritative session ID to the guest");
    expect(host.localPlayerId() == kHostPlayerId && guest.localPlayerId() == kGuestPlayerId,
        "network launch assigns distinct local player IDs");
    expect(host.takeTransport() != nullptr && guest.takeTransport() != nullptr,
        "completed bootstraps hand their connected transports to the session layer");

    NetworkBootstrap mismatchHost;
    expect(mismatchHost.start(hostOptions, contentDigest, sessionId), "mismatch host listens on an available test port");
    guestOptions.port = mismatchHost.port();
    guestOptions.expectedHostIdentity.reset();
    NetworkBootstrap mismatchGuest;
    expect(mismatchGuest.start(guestOptions, contentDigest + 1), "mismatch guest reaches the host");
    pollBootstraps(mismatchHost, mismatchGuest);
    expect(mismatchHost.state() == NetworkBootstrapState::Rejected
            && mismatchGuest.state() == NetworkBootstrapState::Rejected,
        "host and guest report a rejected content mismatch");
    expect(mismatchHost.rejection() == HandshakeRejection::ContentMismatch
            && mismatchGuest.rejection() == HandshakeRejection::ContentMismatch,
        "content mismatch preserves the rejection reason on both peers");

    NetworkBootstrap identityHost;
    expect(identityHost.start(hostOptions, contentDigest, sessionId), "identity-mismatch host listens on an available test port");
    TransportPeerIdentity wrongIdentity = identityHost.localIdentity().value_or(TransportPeerIdentity {});
    wrongIdentity.front() ^= 1;
    guestOptions.port = identityHost.port();
    guestOptions.expectedHostIdentity = wrongIdentity;
    NetworkBootstrap identityGuest;
    expect(identityGuest.start(guestOptions, contentDigest), "identity-mismatch guest establishes its TCP socket");
    pollBootstraps(identityHost, identityGuest);
    expect(identityGuest.state() == NetworkBootstrapState::Failed
            && identityGuest.error() == NetworkBootstrapError::HostIdentityMismatch,
        "guest reports a specific failure when the host certificate fingerprint differs");
}

void pollNetworkLobbies(NetworkLobby& host, NetworkLobby& guest)
{
    for (int attempt = 0; attempt < 10000; attempt++) {
        host.poll();
        guest.poll();
        if (host.state() != NetworkLobbyState::Waiting && guest.state() != NetworkLobbyState::Waiting) {
            return;
        }
    }
}

void testNetworkCombatTurnTransport()
{
    SessionId sessionId { 0xACCE5510ULL };
    LoopbackTransportPair pair = createLoopbackTransportPair();
    NetworkLobby host;
    NetworkLobby guest;
    expect(host.start(NetworkLaunchMode::Host, sessionId, std::move(pair.first))
            && guest.start(NetworkLaunchMode::Join, sessionId, std::move(pair.second)),
        "combat transport peers start");
    expect(host.submitLocalSheet(sampleCharacterSheet(kHostPlayerId, "Host"))
            == CharacterLobbyError::None
            && guest.submitLocalSheet(sampleCharacterSheet(kGuestPlayerId, "Guest"))
                == CharacterLobbyError::None,
        "combat transport peers submit character sheets");
    pollNetworkLobbies(host, guest);
    expect(host.requestStart(), "combat transport host starts world");
    for (int attempt = 0; attempt < 100 && !guest.startRequested(); attempt++) {
        host.poll();
        guest.poll();
    }
    expect(host.startRequested() && guest.startRequested(),
        "combat transport peers enter world");

    CombatTurnState state;
    state.revision = 2;
    state.round = 1;
    state.activeIndex = 1;
    state.remainingMilliseconds = 60000;
    state.initiative = {
        { EntityId { 1 }, kHostPlayerId },
        { EntityId { 2 }, kGuestPlayerId },
        { EntityId { 3 }, {} },
    };
    expect(host.publishDeferredEvent(GameEvent {
               {}, CommandSequence { 1 },
               CombatTurnStateChangedEvent {
                   EntityId { 1 }, SessionPhase::Combat, 4, state,
               },
           }),
        "host publishes authoritative guest turn");
    guest.poll();
    std::optional<GameEvent> begun = guest.takePeerEvent();
    const auto* begunState = begun
        ? std::get_if<CombatTurnStateChangedEvent>(&begun->payload) : nullptr;
    expect(begunState != nullptr && begunState->state.revision == 2
            && begunState->state.initiative[begunState->state.activeIndex].ownerId
                == kGuestPlayerId,
        "guest receives ordered combat turn over live transport");
    expect(begun && guest.confirmPeerEventApplied(begun->sequence),
        "guest confirms combat turn event");

    expect(guest.sendLocalEndTurn(2, 4),
        "guest sends semantic end-turn through command channel");
    host.poll();
    std::optional<GameCommand> command = host.takePeerCommand();
    const auto* endTurn = command
        ? std::get_if<EndTurnCommand>(&command->payload) : nullptr;
    expect(command && command->playerId == kGuestPlayerId
            && command->actorId == EntityId { 2 }
            && command->expectedPhase == SessionPhase::Combat
            && command->expectedPhaseRevision == 4
            && endTurn != nullptr && endTurn->turnRevision == 2,
        "host receives owner and revision keyed end-turn intent");
    if (!command) {
        return;
    }
    state.revision = 3;
    state.activeIndex = 2;
    AuthoritativeCommandResult outcome;
    outcome.result.commandSequence = command->sequence;
    outcome.result.status = CommandStatus::Accepted;
    outcome.result.rejection = CommandRejection::None;
    outcome.event = GameEvent {
        {}, command->sequence,
        CombatTurnStateChangedEvent {
            EntityId { 2 }, SessionPhase::Combat, 4, state,
        },
    };
    expect(host.sendCommandOutcome(std::move(outcome)),
        "host publishes accepted end-turn and next AI turn");
    guest.poll();
    std::optional<CommandResult> result = guest.takeCommandResult();
    std::optional<GameEvent> next = guest.takePeerEvent();
    const auto* nextState = next
        ? std::get_if<CombatTurnStateChangedEvent>(&next->payload) : nullptr;
    expect(result && result->status == CommandStatus::Accepted
            && nextState != nullptr && nextState->state.revision == 3
            && nextState->state.initiative[nextState->state.activeIndex].ownerId.value == 0,
        "guest receives confirmed transition to host AI without simulating it");
    expect(next && guest.confirmPeerEventApplied(next->sequence),
        "guest confirms next combat turn event");
}

void testNetworkCharacterLobby()
{
    SessionId sessionId { 0x1029384756ABCDEFULL };
    LoopbackTransportPair pair = createLoopbackTransportPair();
    NetworkLobby host;
    NetworkLobby guest;
    expect(host.start(NetworkLaunchMode::Host, sessionId, std::move(pair.first)), "network host starts its character lobby");
    expect(guest.start(NetworkLaunchMode::Join, sessionId, std::move(pair.second)), "network guest starts its character lobby");

    expect(!host.sendChatMessage("")
            && !host.sendChatMessage(std::string(kMaxLobbyChatMessageLength + 1, 'x'))
            && !host.sendChatMessage("invalid\nmessage"),
        "network lobby rejects empty, oversized, and control-character chat");
    expect(host.sendChatMessage("Welcome to Vault 13."), "host sends a bounded lobby chat message");
    guest.poll();
    std::optional<LobbyChatMessage> hostChat = guest.takeChatMessage();
    expect(hostChat.has_value()
            && hostChat->playerId == kHostPlayerId
            && hostChat->text == "Welcome to Vault 13."
            && !guest.takeChatMessage().has_value(),
        "guest receives host chat with authenticated player ownership exactly once");
    expect(guest.sendChatMessage("Ready when you are."), "guest sends a lobby chat reply");
    host.poll();
    std::optional<LobbyChatMessage> guestChat = host.takeChatMessage();
    expect(guestChat.has_value()
            && guestChat->playerId == kGuestPlayerId
            && guestChat->text == "Ready when you are.",
        "host receives guest chat with the guest player identity");

    CharacterCreationSheet hostSheet = sampleCharacterSheet(kHostPlayerId, "Albert");
    CharacterCreationSheet guestSheet = sampleCharacterSheet(kGuestPlayerId, "Max");
    expect(host.submitLocalSheet(hostSheet) == CharacterLobbyError::None, "network host sends its character sheet");
    expect(guest.submitLocalSheet(guestSheet) == CharacterLobbyError::None, "network guest sends its character sheet");
    pollNetworkLobbies(host, guest);

    expect(host.state() == NetworkLobbyState::Ready && guest.state() == NetworkLobbyState::Ready,
        "host approval readies both network lobbies");
    expect(host.localSheet() != nullptr && *host.localSheet() == hostSheet
            && host.peerSheet() != nullptr && *host.peerSheet() == guestSheet,
        "host retains both validated character sheets");
    expect(guest.localSheet() != nullptr && *guest.localSheet() == guestSheet
            && guest.peerSheet() != nullptr && *guest.peerSheet() == hostSheet,
        "guest retains both validated character sheets");
    expect(host.submitLocalSheet(hostSheet) == CharacterLobbyError::WrongPhase,
        "ready lobby locks the submitted character sheet");
    expect(!guest.requestStart(), "guest cannot start the network game");
    expect(host.requestStart(), "ready host broadcasts the game start");
    for (int attempt = 0; attempt < 100 && !guest.startRequested(); attempt++) {
        host.poll();
        guest.poll();
    }
    expect(host.startRequested() && guest.startRequested(),
        "host and guest observe the same game start");
    expect(host.sendChatMessage("Meet me by the vault door."),
        "host sends chat after the game starts");
    guest.poll();
    std::optional<LobbyChatMessage> startedChat = guest.takeChatMessage();
    expect(startedChat.has_value()
            && startedChat->playerId == kHostPlayerId
            && startedChat->text == "Meet me by the vault door.",
        "guest receives authenticated chat after the game starts");
    expect(host.sendLocalMove(12345, 0, true, 12340, { 1, 2, 3 }), "host sends its local movement");
    guest.poll();
    std::optional<GameEvent> hostMoveEvent = guest.takePeerEvent();
    const auto* hostMove = hostMoveEvent.has_value()
        ? std::get_if<ActorMovementStartedEvent>(&hostMoveEvent->payload)
        : nullptr;
    expect(hostMove != nullptr
            && hostMove->actorId == EntityId { kHostPlayerId.value }
            && hostMove->destinationTile == 12345
            && hostMove->elevation == 0
            && hostMove->running
            && hostMove->startingTile == 12340
            && hostMove->path == std::vector<std::uint8_t>({ 1, 2, 3 }),
        "guest receives the host movement");
    expect(hostMoveEvent.has_value() && guest.confirmPeerEventApplied(hostMoveEvent->sequence),
        "guest confirms the first event after applying it");
    host.poll();
    expect(host.acknowledgedEvent(kHostPlayerId) == EventSequence { 1 }
            && host.acknowledgedEvent(kGuestPlayerId) == EventSequence { 1 },
        "host tracks its own and the guest's applied boundaries independently");
    expect(!guest.takePeerEvent().has_value(), "received host movement is consumed once");
    expect(guest.sendLocalMove(12346, 1, false), "guest sends its local movement");
    host.poll();
    std::optional<GameCommand> guestMoveCommand = host.takePeerCommand();
    const auto* guestMoveIntent = guestMoveCommand.has_value()
        ? std::get_if<MoveCommand>(&guestMoveCommand->payload)
        : nullptr;
    expect(guestMoveIntent != nullptr
            && guestMoveCommand->playerId == kGuestPlayerId
            && guestMoveCommand->actorId == EntityId { kGuestPlayerId.value }
            && guestMoveIntent->destinationTile == 12346
            && guestMoveIntent->elevation == 1
            && !guestMoveIntent->running,
        "host receives the guest movement command");
    AuthoritativeCommandResult guestMoveOutcome;
    guestMoveOutcome.result.commandSequence = guestMoveCommand->sequence;
    guestMoveOutcome.result.status = CommandStatus::Accepted;
    guestMoveOutcome.result.rejection = CommandRejection::None;
    guestMoveOutcome.event = GameEvent {
        EventSequence { 1 },
        guestMoveCommand->sequence,
        ActorMovementStartedEvent { EntityId { kGuestPlayerId.value }, 12346, 1, false },
    };
    expect(host.sendCommandOutcome(std::move(guestMoveOutcome)), "host publishes the accepted guest movement");
    guest.poll();
    std::optional<CommandResult> guestMoveResult = guest.takeCommandResult();
    std::optional<GameEvent> guestMoveEvent = guest.takePeerEvent();
    const auto* guestMove = guestMoveEvent.has_value()
        ? std::get_if<ActorMovementStartedEvent>(&guestMoveEvent->payload)
        : nullptr;
    expect(guestMoveResult.has_value()
            && guestMoveResult->status == CommandStatus::Accepted
            && guestMoveResult->firstEventSequence == EventSequence { 2 }
            && guestMove != nullptr
            && guestMoveEvent->sequence == EventSequence { 2 }
            && guestMove->actorId == EntityId { kGuestPlayerId.value }
            && guestMove->destinationTile == 12346
            && guestMove->elevation == 1
            && !guestMove->running,
        "guest receives the host-authoritative movement result and event");
    expect(guestMoveEvent.has_value() && guest.confirmPeerEventApplied(guestMoveEvent->sequence),
        "guest advances its applied boundary after authoritative movement");
    expect(host.sendLocalDoorUse(EntityId { 77 }), "host sends its local door use");
    guest.poll();
    std::optional<GameEvent> doorEvent = guest.takePeerEvent();
    const auto* doorUse = doorEvent.has_value()
        ? std::get_if<DoorUseStartedEvent>(&doorEvent->payload)
        : nullptr;
    expect(doorUse != nullptr
            && doorUse->actorId == EntityId { kHostPlayerId.value }
            && doorUse->targetId == EntityId { 77 },
        "guest receives the host door use");
    expect(doorEvent.has_value() && guest.confirmPeerEventApplied(doorEvent->sequence),
        "guest advances its applied boundary after door use");
    expect(guest.sendLocalFacing(4), "guest sends its local facing direction");
    host.poll();
    std::optional<GameCommand> facingCommand = host.takePeerCommand();
    const auto* facingIntent = facingCommand.has_value()
        ? std::get_if<FaceCommand>(&facingCommand->payload)
        : nullptr;
    expect(facingIntent != nullptr && facingIntent->rotation == 4,
        "host receives the guest facing command");
    AuthoritativeCommandResult facingOutcome;
    facingOutcome.result.commandSequence = facingCommand->sequence;
    facingOutcome.result.status = CommandStatus::Accepted;
    facingOutcome.result.rejection = CommandRejection::None;
    facingOutcome.event = GameEvent {
        EventSequence { 1 },
        facingCommand->sequence,
        ActorFacingChangedEvent { EntityId { kGuestPlayerId.value }, 4 },
    };
    expect(host.sendCommandOutcome(std::move(facingOutcome)), "host publishes the accepted guest facing");
    guest.poll();
    std::optional<CommandResult> facingResult = guest.takeCommandResult();
    std::optional<GameEvent> facingEvent = guest.takePeerEvent();
    const auto* facing = facingEvent.has_value()
        ? std::get_if<ActorFacingChangedEvent>(&facingEvent->payload)
        : nullptr;
    expect(facingResult.has_value()
            && facingResult->status == CommandStatus::Accepted
            && facing != nullptr
            && facing->actorId == EntityId { kGuestPlayerId.value }
            && facing->rotation == 4,
        "guest receives the host-authoritative facing result and event");
    expect(facingEvent.has_value() && guest.confirmPeerEventApplied(facingEvent->sequence),
        "guest advances its applied boundary after facing");
    expect(host.sendLocalPickup(EntityId { 88 }), "host sends its local ground-item pickup");
    guest.poll();
    std::optional<GameEvent> hostPickupEvent = guest.takePeerEvent();
    const auto* hostPickup = hostPickupEvent.has_value()
        ? std::get_if<ItemPickupStartedEvent>(&hostPickupEvent->payload)
        : nullptr;
    expect(hostPickup != nullptr
            && hostPickup->actorId == EntityId { kHostPlayerId.value }
            && hostPickup->targetId == EntityId { 88 },
        "guest receives the host pickup with shared actor and item IDs");
    expect(hostPickupEvent.has_value() && guest.confirmPeerEventApplied(hostPickupEvent->sequence),
        "guest advances its applied boundary after the host pickup");
    expect(guest.sendLocalPickup(EntityId { 89 }), "guest sends a ground-item pickup command");
    host.poll();
    std::optional<GameCommand> pickupCommand = host.takePeerCommand();
    const auto* pickupIntent = pickupCommand.has_value()
        ? std::get_if<PickupCommand>(&pickupCommand->payload)
        : nullptr;
    expect(pickupIntent != nullptr
            && pickupCommand->playerId == kGuestPlayerId
            && pickupCommand->actorId == EntityId { kGuestPlayerId.value }
            && pickupIntent->targetId == EntityId { 89 },
        "host receives the guest pickup intent without a client object pointer");
    AuthoritativeCommandResult pickupOutcome;
    pickupOutcome.result.commandSequence = pickupCommand->sequence;
    pickupOutcome.result.status = CommandStatus::Accepted;
    pickupOutcome.result.rejection = CommandRejection::None;
    pickupOutcome.event = GameEvent {
        EventSequence { 1 },
        pickupCommand->sequence,
        ItemPickupStartedEvent { EntityId { kGuestPlayerId.value }, EntityId { 89 } },
    };
    expect(host.sendCommandOutcome(std::move(pickupOutcome)),
        "host publishes an accepted guest pickup");
    guest.poll();
    std::optional<CommandResult> pickupResult = guest.takeCommandResult();
    std::optional<GameEvent> guestPickupEvent = guest.takePeerEvent();
    const auto* guestPickup = guestPickupEvent.has_value()
        ? std::get_if<ItemPickupStartedEvent>(&guestPickupEvent->payload)
        : nullptr;
    expect(pickupResult.has_value()
            && pickupResult->status == CommandStatus::Accepted
            && pickupResult->firstEventSequence == EventSequence { 6 }
            && guestPickup != nullptr
            && guestPickupEvent->sequence == EventSequence { 6 }
            && guestPickup->actorId == EntityId { kGuestPlayerId.value }
            && guestPickup->targetId == EntityId { 89 },
        "guest receives the host-authoritative pickup result and event");
    expect(guestPickupEvent.has_value() && guest.confirmPeerEventApplied(guestPickupEvent->sequence),
        "guest advances its applied boundary after its accepted pickup");
    expect(host.sendLocalLoot(EntityId { 90 }), "host sends its local loot interaction");
    guest.poll();
    std::optional<GameEvent> lootStartedEvent = guest.takePeerEvent();
    const auto* lootStarted = lootStartedEvent.has_value()
        ? std::get_if<LootStartedEvent>(&lootStartedEvent->payload)
        : nullptr;
    expect(lootStarted != nullptr
            && lootStarted->actorId == EntityId { kHostPlayerId.value }
            && lootStarted->targetId == EntityId { 90 }
            && guest.confirmPeerEventApplied(lootStartedEvent->sequence),
        "guest accepts and confirms the host loot event");
    expect(host.sendLocalInventoryTransfer(EntityId { 90 }, EntityId { 1 }, EntityId { 91 }, 2, 2,
               1, {}, ItemDescriptor { 40, 0, 0, 0 }),
        "host publishes an inventory transfer made in the loot window");
    guest.poll();
    std::optional<GameEvent> liveTransferEvent = guest.takePeerEvent();
    const auto* liveTransfer = liveTransferEvent.has_value()
        ? std::get_if<InventoryTransferredEvent>(&liveTransferEvent->payload)
        : nullptr;
    expect(liveTransfer != nullptr
            && liveTransfer->sourceId == EntityId { 90 }
            && liveTransfer->destinationId == EntityId { 1 }
            && liveTransfer->itemId == EntityId { 91 }
            && liveTransfer->quantity == 2
            && guest.confirmPeerEventApplied(liveTransferEvent->sequence),
        "guest accepts and confirms the authoritative loot transfer");
    expect(host.sendLocalItemDrop(EntityId { 1 }, EntityId { 91 }, 1, 3,
               1, EntityId { 92 }, 12345, 0, ItemDescriptor { 40, 0, 0, 0 }),
        "host publishes a local inventory drop");
    guest.poll();
    std::optional<GameEvent> liveDropEvent = guest.takePeerEvent();
    const auto* liveDrop = liveDropEvent.has_value()
        ? std::get_if<ItemDroppedEvent>(&liveDropEvent->payload)
        : nullptr;
    expect(liveDrop != nullptr
            && liveDrop->actorId == EntityId { 1 }
            && liveDrop->sourceId == EntityId { 1 }
            && liveDrop->itemId == EntityId { 91 }
            && liveDrop->remainderItemId == EntityId { 92 }
            && liveDrop->tile == 12345
            && guest.confirmPeerEventApplied(liveDropEvent->sequence),
        "guest accepts and confirms the authoritative ground drop");
    expect(host.sendLocalSkillUse(EntityId { 77 }, ExplorationSkill::Traps),
        "host publishes a targeted exploration skill cue");
    guest.poll();
    std::optional<GameEvent> liveSkillEvent = guest.takePeerEvent();
    const auto* liveSkill = liveSkillEvent.has_value()
        ? std::get_if<SkillUseStartedEvent>(&liveSkillEvent->payload)
        : nullptr;
    expect(liveSkill != nullptr
            && liveSkill->actorId == EntityId { kHostPlayerId.value }
            && liveSkill->targetId == EntityId { 77 }
            && liveSkill->skill == ExplorationSkill::Traps
            && guest.confirmPeerEventApplied(liveSkillEvent->sequence),
        "guest accepts and confirms the host skill-use boundary");
    host.poll();
    expect(host.acknowledgedEvent(kHostPlayerId) == EventSequence { 10 }
            && host.acknowledgedEvent(kGuestPlayerId) == EventSequence { 10 },
        "participant acknowledgements advance monotonically through live events");
    EventReplay completeReplay = host.replayAfter(EventSequence {});
    expect(completeReplay.status == EventReplayStatus::Available
            && completeReplay.events.size() == 10
            && completeReplay.events.front().sequence == EventSequence { 1 }
            && completeReplay.events.back().sequence == EventSequence { 10 },
        "host journals every authoritative live event in session order");
    expect(guest.requestRecovery(EventSequence { 6 }), "guest requests recovery from its last retained event");
    host.poll();
    std::optional<EventSequence> recoveryRequest = host.takeRecoveryRequest();
    expect(recoveryRequest == EventSequence { 6 }, "host receives the guest recovery boundary");
    WorldSnapshot recoverySnapshot = sampleSnapshot();
    recoverySnapshot.lastIncludedEvent = host.latestAuthoritativeEvent();
    expect(host.sendRecovery(*recoveryRequest, recoverySnapshot), "host sends retained journal events for a short recovery gap");
    guest.poll();
    std::optional<GameEvent> replayedLoot = guest.takePeerEvent();
    std::optional<GameEvent> replayedTransfer = guest.takePeerEvent();
    std::optional<GameEvent> replayedDrop = guest.takePeerEvent();
    std::optional<GameEvent> replayedSkill = guest.takePeerEvent();
    expect(replayedLoot.has_value()
            && replayedLoot->sequence == EventSequence { 7 }
            && std::holds_alternative<LootStartedEvent>(replayedLoot->payload)
            && replayedTransfer.has_value()
            && replayedTransfer->sequence == EventSequence { 8 }
            && std::holds_alternative<InventoryTransferredEvent>(replayedTransfer->payload)
            && replayedDrop.has_value()
            && replayedDrop->sequence == EventSequence { 9 }
            && std::holds_alternative<ItemDroppedEvent>(replayedDrop->payload)
            && replayedSkill.has_value()
            && replayedSkill->sequence == EventSequence { 10 }
            && std::holds_alternative<SkillUseStartedEvent>(replayedSkill->payload)
            && !guest.recoveryInProgress(),
        "guest receives ordered loot, drop, and skill replay and observes recovery completion");

    EventSequence guestResumePoint = guest.lastAppliedEvent();
    expect(guestResumePoint == EventSequence { 10 } && guest.disconnectForReconnect(),
        "guest records its last applied event and drops the old connection");
    host.poll();
    expect(host.state() == NetworkLobbyState::Disconnected
            && guest.state() == NetworkLobbyState::Disconnected,
        "both lobbies enter reconnectable state after the socket closes");
    expect(!guest.sendLocalMove(12347, 0, false),
        "disconnected guest input is blocked");
    expect(host.sendLocalFacing(2)
            && host.latestAuthoritativeEvent() == EventSequence { 11 },
        "host continues the authoritative journal while the guest is absent");
    expect(host.acknowledgedEvent(kHostPlayerId) == EventSequence { 11 }
            && host.acknowledgedEvent(kGuestPlayerId) == EventSequence { 10 },
        "a disconnected guest does not rewind or stall the host's applied boundary");

    LoopbackTransportPair resumedPair = createLoopbackTransportPair();
    expect(host.reattachTransport(std::move(resumedPair.first))
            && guest.reattachTransport(std::move(resumedPair.second)),
        "both lobbies attach a freshly authenticated transport");
    expect(host.queueRecovery(guestResumePoint)
            && guest.beginReconnectRecovery(guestResumePoint),
        "reconnect starts recovery from the handshake event boundary");
    std::optional<EventSequence> reconnectRecovery = host.takeRecoveryRequest();
    recoverySnapshot.lastIncludedEvent = host.latestAuthoritativeEvent();
    bool reconnectSent = reconnectRecovery.has_value()
        && reconnectRecovery == guestResumePoint
        && host.sendRecovery(*reconnectRecovery, recoverySnapshot);
    expect(reconnectSent,
        "host resumes the reconnect through its retained event journal");
    guest.poll();
    std::optional<GameEvent> resumedFacing = guest.takePeerEvent();
    const auto* resumedFacingPayload = resumedFacing.has_value()
        ? std::get_if<ActorFacingChangedEvent>(&resumedFacing->payload)
        : nullptr;
    expect(resumedFacingPayload != nullptr
            && resumedFacing->sequence == EventSequence { 11 }
            && resumedFacingPayload->rotation == 2
            && !guest.recoveryInProgress(),
        "guest applies events created while disconnected and completes reconnect recovery");
    expect(resumedFacing.has_value() && guest.confirmPeerEventApplied(resumedFacing->sequence)
            && guest.lastAppliedEvent() == EventSequence { 11 },
        "guest confirms the replayed event as its new reconnect boundary");
    host.poll();
    expect(host.acknowledgedEvent(kGuestPlayerId) == EventSequence { 11 },
        "reconnected guest acknowledgement advances only its own boundary");
    WorldSnapshot authoritativeState = sampleSnapshot();
    expect(host.sendAuthoritativeState(authoritativeState),
        "host sends a non-journaled full authoritative correction");
    guest.poll();
    std::optional<WorldSnapshot> receivedState = guest.takeAuthoritativeState();
    expect(receivedState.has_value()
            && receivedState->actors.size() == 2
            && receivedState->critters.size() == 1
            && receivedState->critters[0].entityId == EntityId { 12 }
            && receivedState->critters[0].hitPoints == 6
            && receivedState->doors.size() == 1
            && receivedState->items.size() == 2,
        "guest receives complete authoritative state independently of replay events");
    expect(host.takeTransport() != nullptr && guest.takeTransport() != nullptr,
        "ready lobbies hand the connection to the game session");

    LoopbackTransportPair wrongPlayerPair = createLoopbackTransportPair();
    NetworkLobby wrongPlayerGuest;
    expect(wrongPlayerGuest.start(NetworkLaunchMode::Join, sessionId, std::move(wrongPlayerPair.first)),
        "guest lobby starts for local validation");
    expect(wrongPlayerGuest.submitLocalSheet(hostSheet) == CharacterLobbyError::InvalidPlayerId,
        "guest cannot submit a host-owned character sheet");

    LoopbackTransportPair rejectedPair = createLoopbackTransportPair();
    NetworkLobby rejectingHost;
    expect(rejectingHost.start(NetworkLaunchMode::Host, sessionId, std::move(rejectedPair.first)),
        "rejecting host lobby starts");
    std::vector<std::uint8_t> encodedWrongSheet;
    expect(encodeCharacterSheet(hostSheet, encodedWrongSheet) == CharacterLobbyError::None,
        "wrong-owner character sheet encodes for hostile packet test");
    ProtocolEnvelope wrongOwnerEnvelope;
    wrongOwnerEnvelope.kind = MessageKind::Lobby;
    wrongOwnerEnvelope.sessionId = sessionId;
    wrongOwnerEnvelope.sequence = 2;
    wrongOwnerEnvelope.payload = { 0, static_cast<std::uint8_t>(kNetworkLobbyVersion), 1, 0 };
    wrongOwnerEnvelope.payload.insert(wrongOwnerEnvelope.payload.end(), encodedWrongSheet.begin(), encodedWrongSheet.end());
    Packet wrongOwnerPacket;
    expect(encodeEnvelope(wrongOwnerEnvelope, wrongOwnerPacket) == ProtocolError::None
            && rejectedPair.second->send(std::move(wrongOwnerPacket)) == TransportSendResult::Sent,
        "hostile peer sends a host-owned sheet in the guest slot");
    rejectingHost.poll();
    expect(rejectingHost.state() == NetworkLobbyState::Rejected
            && rejectingHost.sheetError() == CharacterLobbyError::InvalidPlayerId,
        "host rejects a peer sheet owned by the wrong player");

    LoopbackTransportPair forgedChatPair = createLoopbackTransportPair();
    NetworkLobby forgedChatHost;
    expect(forgedChatHost.start(NetworkLaunchMode::Host, sessionId, std::move(forgedChatPair.first)),
        "host lobby starts for forged chat validation");
    ProtocolEnvelope forgedChatEnvelope;
    forgedChatEnvelope.kind = MessageKind::Lobby;
    forgedChatEnvelope.sessionId = sessionId;
    forgedChatEnvelope.sequence = 2;
    forgedChatEnvelope.payload = {
        0,
        static_cast<std::uint8_t>(kNetworkLobbyVersion),
        5,
        0,
        0,
        0,
        0,
        static_cast<std::uint8_t>(kHostPlayerId.value),
        0,
        1,
        'x',
    };
    Packet forgedChatPacket;
    expect(encodeEnvelope(forgedChatEnvelope, forgedChatPacket) == ProtocolError::None
            && forgedChatPair.second->send(std::move(forgedChatPacket)) == TransportSendResult::Sent,
        "hostile guest sends chat claiming the host identity");
    forgedChatHost.poll();
    expect(forgedChatHost.state() == NetworkLobbyState::Failed
            && forgedChatHost.error() == NetworkLobbyError::UnexpectedMessage,
        "lobby rejects chat whose player identity does not match the peer role");

    LoopbackTransportPair disconnectedPair = createLoopbackTransportPair();
    NetworkLobby disconnectedHost;
    NetworkLobby disconnectedGuest;
    expect(disconnectedHost.start(NetworkLaunchMode::Host, sessionId, std::move(disconnectedPair.first)),
        "disconnect test host lobby starts");
    expect(disconnectedGuest.start(NetworkLaunchMode::Join, sessionId, std::move(disconnectedPair.second)),
        "disconnect test guest lobby starts");
    disconnectedGuest.stop();
    disconnectedHost.poll();
    expect(disconnectedHost.state() == NetworkLobbyState::Failed
            && disconnectedHost.error() == NetworkLobbyError::Disconnected,
        "waiting lobby reports a disconnected peer");
}

void testLocalSessionLifecycle()
{
    LocalSession session;
    TestObject hostActor;
    TestObject guestActor;
    TestObject replacementGuest;

    expect(!session.isActive(), "local session starts inactive");
    expect(session.phaseRevision() == 0, "inactive session has no phase revision");
    expect(session.submitCharacterSheet(sampleCharacterSheet(kHostPlayerId, "Host")) == CharacterLobbyError::SessionInactive, "inactive session rejects character sheets");
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

    expect(session.transitionTo(SessionPhase::Combat) == LocalSessionError::InvalidTransition, "lobby cannot jump directly to combat");
    expect(session.phaseRevision() == 1, "rejected transition does not change phase revision");
    expect(session.transitionTo(SessionPhase::Loading) == LocalSessionError::LobbyNotReady, "lobby cannot load before both character sheets pass validation");
    expect(session.submitCharacterSheet(sampleCharacterSheet(kHostPlayerId, "Host")) == CharacterLobbyError::None, "local session accepts the host character sheet");
    expect(!session.characterLobbyReady() && session.characterLobby().isReady(kHostPlayerId), "one accepted sheet does not ready the two-player lobby");
    expect(session.transitionTo(SessionPhase::Loading) == LocalSessionError::LobbyNotReady, "one ready player cannot leave the lobby");
    expect(session.submitCharacterSheet(sampleCharacterSheet(kGuestPlayerId, "Guest")) == CharacterLobbyError::None, "local session accepts the guest character sheet");
    expect(session.characterLobbyReady(), "both accepted character sheets ready the lobby");
    expect(session.characterLobby().sheet(kGuestPlayerId) != nullptr && session.characterLobby().sheet(kGuestPlayerId)->name == "Guest", "lobby retains the accepted guest sheet");
    expect(session.characterLobby().sheet(PlayerId { 99 }) == nullptr, "lobby has no sheet for an unknown player");
    expect(session.players().find(kHostPlayerId)->name == "Host" && session.players().find(kGuestPlayerId)->name == "Guest", "accepted sheets assign separate character names");

    CharacterBuild guestBuild = session.players().find(kGuestPlayerId)->build;
    guestBuild.baseStats[0] = 6;
    guestBuild.level = 3;
    expect(session.players().setBuild(kGuestPlayerId, guestBuild) == PlayerStateError::None, "local session accepts a guest character build");

    expect(session.transitionTo(SessionPhase::Loading) == LocalSessionError::None, "lobby can enter loading");
    expect(session.submitCharacterSheet(sampleCharacterSheet(kGuestPlayerId, "Late")) == CharacterLobbyError::WrongPhase, "character sheets cannot change after leaving the lobby");
    expect(session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None, "loading can enter exploration");
    expect(session.phaseRevision() == 3, "accepted transitions advance the phase revision");
    expect(session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None, "repeating the current phase is harmless");
    expect(session.phaseRevision() == 3, "repeating the current phase does not advance its revision");
    expect(session.applyAuthoritativePhase(SessionPhase::Combat, 7) == LocalSessionError::None
            && session.phase() == SessionPhase::Combat
            && session.phaseRevision() == 7,
        "an authoritative snapshot can advance a guest across missed phase revisions");
    expect(session.applyAuthoritativePhase(SessionPhase::Exploration, 7) == LocalSessionError::InvalidTransition,
        "an authoritative phase cannot change without a newer revision");
    expect(session.applyAuthoritativePhase(SessionPhase::Exploration, 6) == LocalSessionError::InvalidTransition,
        "an authoritative phase cannot roll a guest back to a stale revision");
    expect(session.applyAuthoritativePhase(SessionPhase::Exploration, 8) == LocalSessionError::None,
        "a newer authoritative revision can return the guest to exploration");

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
    expect(!session.characterLobbyReady(), "stopping clears lobby readiness");
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

    CommandExecutionStatus face(Object* actor, const FaceCommand& command) override
    {
        faceCalls++;
        lastActor = actor;
        lastFace = command;
        recordContext();
        return nextStatus;
    }

    DoorUseExecution useDoor(Object* actor, Object* target) override
    {
        doorCalls++;
        lastActor = actor;
        lastTarget = target;
        recordContext();
        return DoorUseExecution { nextStatus, true, false, 3 };
    }

    CommandExecutionStatus pickup(Object* actor, Object* target) override
    {
        pickupCalls++;
        lastActor = actor;
        lastTarget = target;
        recordContext();
        if (reservePickups && !reservedPickupTargets.insert(target).second) {
            return CommandExecutionStatus::InvalidAction;
        }
        return nextStatus;
    }

    CommandExecutionStatus loot(Object* actor, Object* target) override
    {
        lootCalls++;
        lastActor = actor;
        lastTarget = target;
        recordContext();
        return nextStatus;
    }

    CommandExecutionStatus useSkill(Object* actor, Object* target, const UseSkillCommand& command) override
    {
        skillCalls++;
        lastActor = actor;
        lastTarget = target;
        lastSkill = command;
        recordContext();
        return nextStatus;
    }

    CommandExecutionStatus useItemOn(Object* actor, Object* item, Object* target, const UseItemOnCommand& command) override
    {
        itemUseCalls++;
        lastActor = actor;
        lastSource = item;
        lastTarget = target;
        lastItemUse = command;
        recordContext();
        return nextStatus;
    }

    ElevatorExecution useElevator(Object* actor, const ElevatorCommand& command) override
    {
        elevatorCalls++;
        lastActor = actor;
        lastElevator = command;
        recordContext();
        return ElevatorExecution { nextStatus, 6, 14105, 0, 3, 22504, 1, 2, 5 };
    }

    ExitGridExecution useExitGrid(Object* actor, Object* target, const ExitGridCommand& command) override
    {
        exitGridCalls++;
        lastActor = actor;
        lastTarget = target;
        lastExitGrid = command;
        recordContext();
        return ExitGridExecution {
            nextStatus,
            35,
            {
                PlayerTransitionPlacement { kHostPlayerId, EntityId { 1 }, 17091, 0, 0 },
                PlayerTransitionPlacement { kGuestPlayerId, EntityId { 2 }, 17090, 0, 0 },
            },
            7,
        };
    }

    SceneryTransitionExecution useSceneryTransition(Object* actor, Object* target, const SceneryTransitionCommand& command) override
    {
        sceneryTransitionCalls++;
        lastActor = actor;
        lastTarget = target;
        lastSceneryTransition = command;
        recordContext();
        return SceneryTransitionExecution {
            nextStatus,
            5,
            {
                PlayerTransitionPlacement { kHostPlayerId, EntityId { 1 }, 15483, 0, 5 },
                PlayerTransitionPlacement { kGuestPlayerId, EntityId { 2 }, 17089, 1, 5 },
            },
            9,
        };
    }

    RestExecution rest(Object* actor, const RestCommand& command) override
    {
        restCalls++;
        lastActor = actor;
        lastRest = command;
        recordContext();
        return RestExecution { nextStatus, true, 231456, 9 };
    }

    CommandExecutionStatus attack(Object* actor, Object* target, const AttackCommand& command) override
    {
        attackCalls++;
        lastActor = actor;
        lastTarget = target;
        lastAttack = command;
        recordContext();
        return nextStatus;
    }

    CommandExecutionStatus combatMove(Object* actor, const CombatMoveCommand& command) override
    {
        combatActionCalls++;
        lastActor = actor;
        lastCombatTurnRevision = command.turnRevision;
        recordContext();
        return combatActionAllowed(actor, command.turnRevision);
    }

    CommandExecutionStatus combatItem(Object* actor, const CombatItemCommand& command) override
    {
        combatActionCalls++;
        lastActor = actor;
        lastCombatTurnRevision = command.turnRevision;
        recordContext();
        return combatActionAllowed(actor, command.turnRevision);
    }

    CommandExecutionStatus combatReload(Object* actor, const CombatReloadCommand& command) override
    {
        combatActionCalls++;
        lastActor = actor;
        lastCombatTurnRevision = command.turnRevision;
        recordContext();
        return combatActionAllowed(actor, command.turnRevision);
    }

    CommandExecutionStatus combatFace(Object* actor, const CombatFaceCommand& command) override
    {
        combatActionCalls++;
        lastActor = actor;
        lastCombatTurnRevision = command.turnRevision;
        recordContext();
        return combatActionAllowed(actor, command.turnRevision);
    }

    CommandExecutionStatus combatActionAllowed(Object* actor, std::uint64_t revision) const
    {
        if (combatTurns == nullptr || modalSession == nullptr) {
            return CommandExecutionStatus::InvalidAction;
        }
        std::optional<EntityId> actorId = modalSession->entities().findEntity(actor);
        const CombatTurnEntry* turn = combatTurns->current();
        return actorId.has_value() && turn != nullptr
                && turn->actorId == *actorId && turn->owner.has_value()
                && turn->owner == modalSession->players().findByActor(*actorId)->id
                && revision == combatTurns->revision()
            ? nextStatus : CommandExecutionStatus::InvalidAction;
    }

    SharedModalExecution setSharedModal(Object* actor, const SharedModalCommand& command) override
    {
        modalCalls++;
        lastActor = actor;
        lastModal = command;
        recordContext();
        if (nextStatus != CommandExecutionStatus::Applied || modalSession == nullptr) {
            return {};
        }
        if (command.open) {
            if (activeModalActor != nullptr
                || modalSession->transitionTo(sharedModalPhase(command.kind)) != LocalSessionError::None) {
                return {};
            }
            activeModalActor = actor;
            activeModalKind = command.kind;
        } else {
            if (activeModalActor != actor
                || activeModalKind != command.kind
                || modalSession->transitionTo(SessionPhase::Exploration) != LocalSessionError::None) {
                return {};
            }
            activeModalActor = nullptr;
        }
        return SharedModalExecution {
            CommandExecutionStatus::Applied,
            modalSession->phase(),
            modalSession->phaseRevision(),
        };
    }

    EndTurnExecution endTurn(Object* actor, PlayerId playerId,
        const EndTurnCommand& command) override
    {
        endTurnCalls++;
        if (combatTurns == nullptr || modalSession == nullptr) {
            return {};
        }
        std::optional<EntityId> actorId = modalSession->entities().findEntity(actor);
        if (!actorId.has_value()
            || combatTurns->endPlayerTurn(playerId, *actorId,
                   command.turnRevision, 100)
                != CombatTurnResult::Accepted) {
            return {};
        }
        return { CommandExecutionStatus::Applied, combatTurns->snapshot(100) };
    }

    InventoryTransferExecution transferInventory(Object* actor,
        Object* source,
        Object* destination,
        Object* item,
        const InventoryTransferCommand& command) override
    {
        transferCalls++;
        lastActor = actor;
        lastSource = source;
        lastDestination = destination;
        lastTarget = item;
        lastQuantity = command.quantity;
        recordContext();
        return InventoryTransferExecution {
            nextStatus,
            isValid(command.itemId) ? command.itemId : EntityId { 88 },
            command.quantity < command.sourceQuantity ? EntityId { 89 } : EntityId {},
            ItemDescriptor { 40, 0, 0, 0 },
        };
    }

    ItemDropExecution dropItem(Object* actor,
        Object* source,
        Object* item,
        const ItemDropCommand& command) override
    {
        dropCalls++;
        lastActor = actor;
        lastSource = source;
        lastTarget = item;
        lastQuantity = command.sourceQuantity;
        recordContext();
        return ItemDropExecution {
            nextStatus,
            isValid(command.itemId) ? command.itemId : EntityId { 90 },
            command.quantity < command.sourceQuantity ? EntityId { 91 } : EntityId {},
            12345,
            1,
            ItemDescriptor { 40, 0, 0, 0 },
        };
    }

    void recordContext()
    {
        PlayerCharacterState* player = actingPlayerState();
        lastActingPlayerId = player != nullptr ? player->id : PlayerId {};
        lastContextActor = actingPlayerActor();
        lastBuild = actingCharacterBuild();
    }

    CommandExecutionStatus nextStatus = CommandExecutionStatus::Applied;
    bool reservePickups = false;
    int moveCalls = 0;
    int faceCalls = 0;
    int doorCalls = 0;
    int pickupCalls = 0;
    int lootCalls = 0;
    int skillCalls = 0;
    int itemUseCalls = 0;
    int elevatorCalls = 0;
    int exitGridCalls = 0;
    int sceneryTransitionCalls = 0;
    int restCalls = 0;
    int attackCalls = 0;
    int combatActionCalls = 0;
    std::uint64_t lastCombatTurnRevision = 0;
    int modalCalls = 0;
    int endTurnCalls = 0;
    int transferCalls = 0;
    int dropCalls = 0;
    Object* lastActor = nullptr;
    Object* lastTarget = nullptr;
    Object* lastSource = nullptr;
    Object* lastDestination = nullptr;
    std::uint32_t lastQuantity = 0;
    Object* lastContextActor = nullptr;
    CharacterBuild* lastBuild = nullptr;
    PlayerId lastActingPlayerId;
    MoveCommand lastMove;
    FaceCommand lastFace;
    AttackCommand lastAttack;
    UseSkillCommand lastSkill;
    UseItemOnCommand lastItemUse;
    ElevatorCommand lastElevator;
    ExitGridCommand lastExitGrid;
    SceneryTransitionCommand lastSceneryTransition;
    RestCommand lastRest;
    SharedModalCommand lastModal;
    LocalSession* modalSession = nullptr;
    CombatTurnController* combatTurns = nullptr;
    Object* activeModalActor = nullptr;
    SharedModalKind activeModalKind = SharedModalKind::Dialogue;
    std::unordered_set<Object*> reservedPickupTargets;
};

void testCombatTurnCommandsAndReplication()
{
    TestObject hostActor;
    TestObject guestActor;
    TestObject aiActor;
    LocalSession session;
    expect(session.start(asGameObject(hostActor), asGameObject(guestActor))
            == LocalSessionError::None,
        "combat command session starts");
    submitBothCharacterSheets(session);
    expect(session.transitionTo(SessionPhase::Loading) == LocalSessionError::None
            && session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None
            && session.transitionTo(SessionPhase::Combat) == LocalSessionError::None,
        "combat command session enters authoritative combat phase");
    EntityRegistrationResult ai = session.registerWorldObject(asGameObject(aiActor));
    CombatTurnController turns;
    expect(turns.begin({
               { session.playerActorId(kHostPlayerId), kHostPlayerId },
               { session.playerActorId(kGuestPlayerId), kGuestPlayerId },
               { ai.entityId, std::nullopt },
           }, 100, 60000)
            == CombatTurnResult::Accepted,
        "combat command test begins ordered host, guest, AI initiative");
    RecordingCommandExecutor executor;
    executor.modalSession = &session;
    executor.combatTurns = &turns;
    CommandProcessor processor;

    GameCommand guest;
    guest.sequence.value = 1;
    guest.playerId = kGuestPlayerId;
    guest.actorId = session.playerActorId(kGuestPlayerId);
    guest.expectedPhase = SessionPhase::Combat;
    guest.expectedPhaseRevision = session.phaseRevision();
    guest.payload = EndTurnCommand { 1 };
    AuthoritativeCommandResult rejected = processor.process(guest, session, executor);
    expect(rejected.result.rejection == CommandRejection::InvalidAction
            && !rejected.event.has_value()
            && turns.revision() == 1
            && turns.current()->actorId == session.playerActorId(kHostPlayerId),
        "out-of-turn guest command cannot mutate host turn or emit an event");

    GameCommand host = guest;
    host.playerId = kHostPlayerId;
    host.actorId = session.playerActorId(kHostPlayerId);
    AuthoritativeCommandResult advanced = processor.process(host, session, executor);
    const auto* hostEvent = advanced.event.has_value()
        ? std::get_if<CombatTurnStateChangedEvent>(&advanced.event->payload)
        : nullptr;
    expect(advanced.result.status == CommandStatus::Accepted
            && hostEvent != nullptr
            && hostEvent->state.revision == 2
            && hostEvent->state.initiative[hostEvent->state.activeIndex].ownerId == kGuestPlayerId,
        "host end-turn publishes next guest-owned turn");
    expect(processor.process(host, session, executor).replayed
            && turns.revision() == 2,
        "replayed end-turn cannot advance twice");

    guest.sequence.value = 2;
    guest.payload = EndTurnCommand { 1 };
    rejected = processor.process(guest, session, executor);
    expect(rejected.result.rejection == CommandRejection::InvalidAction
            && turns.revision() == 2,
        "stale turn revision is rejected without changing initiative");
    guest.sequence.value = 3;
    guest.payload = EndTurnCommand { 2 };
    advanced = processor.process(guest, session, executor);
    expect(advanced.result.status == CommandStatus::Accepted
            && turns.current()->actorId == ai.entityId,
        "guest end-turn advances to host AI");

    ProtocolEnvelope envelope = sampleEnvelope();
    envelope.sessionId.value = 5;
    envelope.sequence = 2;
    expect(encodeGameCommand(guest, envelope) == GameplayWireError::None,
        "semantic end-turn command encodes");
    GameCommandDecodeResult decodedCommand = decodeGameCommand(envelope);
    expect(decodedCommand && std::get<EndTurnCommand>(decodedCommand.command.payload).turnRevision == 2,
        "semantic end-turn revision survives wire round-trip");

    GameEvent event;
    event.sequence.value = 9;
    event.causedBy.value = 3;
    event.payload = CombatTurnStateChangedEvent {
        guest.actorId, SessionPhase::Combat, session.phaseRevision(),
        turns.snapshot(100),
    };
    expect(encodeGameEvent(event, envelope) == GameplayWireError::None,
        "combat turn state event encodes");
    GameEventDecodeResult decodedEvent = decodeGameEvent(envelope);
    const auto* replicated = decodedEvent
        ? std::get_if<CombatTurnStateChangedEvent>(&decodedEvent.event.payload)
        : nullptr;
    expect(replicated != nullptr
            && replicated->state.revision == turns.revision()
            && replicated->state.initiative.size() == 3
            && replicated->state.initiative[replicated->state.activeIndex].actorId == ai.entityId,
        "combat initiative and active actor survive event wire round-trip");

    WorldSnapshot snapshot = sampleSnapshot();
    snapshot.phase = SessionPhase::Combat;
    snapshot.combat = turns.snapshot(100);
    snapshot.combatFreeMove = 4;
    snapshot.combat.initiative[0].actorId = snapshot.actors[1].entityId;
    snapshot.combat.initiative[1].actorId = snapshot.actors[0].entityId;
    snapshot.combat.initiative[2].actorId = snapshot.critters[0].entityId;
    expect(validateSnapshot(snapshot) == SnapshotError::None,
        "combat recovery snapshot accepts owned player and AI roster");
    std::vector<std::uint8_t> packet;
    expect(encodeSnapshot(snapshot, packet) == SnapshotError::None,
        "combat recovery snapshot encodes");
    SnapshotDecodeResult decodedSnapshot = decodeSnapshot(packet);
    expect(decodedSnapshot
            && decodedSnapshot.snapshot.combat.revision == turns.revision()
            && decodedSnapshot.snapshot.combat.initiative.size() == 3
            && decodedSnapshot.snapshot.combatFreeMove == 4,
        "combat recovery snapshot restores initiative, turn revision, and free movement");
    WorldSnapshot statusSnapshot = snapshot;
    statusSnapshot.actors[0].combatResults = DAM_KNOCKED_OUT;
    statusSnapshot.actors[0].elevation = 1;
    statusSnapshot.critters[0].combatResults = DAM_DEAD;
    statusSnapshot.critters[0].hitPoints = 0;
    statusSnapshot.critters[0].combatManeuver = CRITTER_MANUEVER_FLEEING;
    expect(encodeSnapshot(statusSnapshot, packet) == SnapshotError::None,
        "combat recovery accepts knockout, death, fleeing, and separated elevations");
    SnapshotDecodeResult statuses = decodeSnapshot(packet);
    expect(statuses
            && statuses.snapshot.actors[1].combatResults == DAM_KNOCKED_OUT
            && statuses.snapshot.actors[1].elevation == 1
            && statuses.snapshot.critters[0].combatResults == DAM_DEAD
            && statuses.snapshot.critters[0].hitPoints == 0
            && statuses.snapshot.critters[0].combatManeuver == CRITTER_MANUEVER_FLEEING,
        "combat status and elevation survive a recovery round-trip");
    snapshot.combat.initiative[1].ownerId = kHostPlayerId;
    expect(validateSnapshot(snapshot) == SnapshotError::InvalidCombatState,
        "combat snapshot rejects mismatched player ownership");
}

void testCombatActionCommandsAndReplication()
{
    TestObject hostActor;
    TestObject guestActor;
    TestObject item;
    TestObject weapon;
    LocalSession session;
    expect(session.start(asGameObject(hostActor), asGameObject(guestActor))
            == LocalSessionError::None,
        "combat action session starts");
    submitBothCharacterSheets(session);
    expect(session.transitionTo(SessionPhase::Loading) == LocalSessionError::None
            && session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None
            && session.transitionTo(SessionPhase::Combat) == LocalSessionError::None,
        "combat action session enters combat");
    EntityId itemId = session.registerWorldObject(asGameObject(item)).entityId;
    EntityId weaponId = session.registerWorldObject(asGameObject(weapon)).entityId;
    CombatTurnController turns;
    expect(turns.begin({
               { session.playerActorId(kHostPlayerId), kHostPlayerId },
               { session.playerActorId(kGuestPlayerId), kGuestPlayerId },
           }, 100, 60000) == CombatTurnResult::Accepted,
        "combat actions begin with a host-owned turn");
    RecordingCommandExecutor executor;
    executor.modalSession = &session;
    executor.combatTurns = &turns;
    CommandProcessor processor;
    GameCommand command;
    command.playerId = kHostPlayerId;
    command.actorId = session.playerActorId(kHostPlayerId);
    command.expectedPhase = SessionPhase::Combat;
    command.expectedPhaseRevision = session.phaseRevision();
    std::array<GameCommandPayload, 4> actions {
        CombatMoveCommand { 1, 12345, 0, false },
        CombatItemCommand { 1, itemId, {} },
        CombatReloadCommand { 1, weaponId, 7 },
        CombatFaceCommand { 1, 3 },
    };
    std::array<CombatActionKind, 4> kinds {
        CombatActionKind::Move, CombatActionKind::UseItem,
        CombatActionKind::Reload, CombatActionKind::Face,
    };
    ProtocolEnvelope envelope = sampleEnvelope();
    for (std::size_t index = 0; index < actions.size(); index++) {
        command.sequence.value = index + 1;
        command.payload = actions[index];
        expect(encodeGameCommand(command, envelope) == GameplayWireError::None,
            "combat action command encodes");
        GameCommandDecodeResult decoded = decodeGameCommand(envelope);
        expect(decoded && decoded.command.payload.index() == command.payload.index(),
            "combat action command round-trips over the wire");
        AuthoritativeCommandResult result = processor.process(command, session, executor);
        const auto* event = result.event.has_value()
            ? std::get_if<CombatActionResolvedEvent>(&result.event->payload) : nullptr;
        expect(result.result.status == CommandStatus::Accepted && event != nullptr
                && event->kind == kinds[index]
                && event->turnRevision == 1
                && event->phaseRevision == session.phaseRevision(),
            "combat action publishes an owned, revision-keyed result");
        expect(executor.lastActor == asGameObject(hostActor)
                && executor.lastActingPlayerId == kHostPlayerId
                && executor.lastCombatTurnRevision == 1,
            "combat action runs in the owned actor context");
        if (event != nullptr) {
            expect(encodeGameEvent(*result.event, envelope) == GameplayWireError::None,
                "combat result event encodes");
            GameEventDecodeResult replicated = decodeGameEvent(envelope);
            const auto* copied = replicated
                ? std::get_if<CombatActionResolvedEvent>(&replicated.event.payload)
                : nullptr;
            expect(copied != nullptr && copied->kind == kinds[index]
                    && copied->subjectId == event->subjectId,
                "combat result event round-trips over the wire");
        }
        expect(processor.process(command, session, executor).replayed
                && executor.combatActionCalls == static_cast<int>(index + 1),
            "replayed combat action does not execute twice");
    }
    command.sequence.value = 1;
    command.playerId = kGuestPlayerId;
    command.actorId = session.playerActorId(kGuestPlayerId);
    command.payload = CombatMoveCommand { 1, 12346, 0, false };
    expect(processor.process(command, session, executor).result.rejection
            == CommandRejection::InvalidAction,
        "out-of-turn combat movement cannot execute");
    command.playerId = kHostPlayerId;
    command.actorId = session.playerActorId(kHostPlayerId);
    command.sequence.value = 5;
    command.payload = CombatMoveCommand { 2, 12346, 0, false };
    expect(processor.process(command, session, executor).result.rejection
            == CommandRejection::InvalidAction,
        "stale combat action revision cannot execute");
    command.sequence.value = 6;
    command.expectedPhaseRevision++;
    command.payload = CombatFaceCommand { 1, 2 };
    expect(processor.process(command, session, executor).result.rejection
            == CommandRejection::Stale,
        "stale session revision rejects combat action before execution");
    command.expectedPhaseRevision--;
    command.expectedPhase = SessionPhase::Exploration;
    command.sequence.value = 7;
    expect(processor.process(command, session, executor).result.rejection
            == CommandRejection::WrongPhase,
        "combat action cannot execute in a noncombat phase");
    command.expectedPhase = SessionPhase::Combat;
    command.sequence.value = 8;
    command.payload = CombatItemCommand { 1, itemId,
        session.playerActorId(kGuestPlayerId) };
    expect(encodeGameCommand(command, envelope) == GameplayWireError::None,
        "targeted combat item command encodes");
    GameCommandDecodeResult targetedItem = decodeGameCommand(envelope);
    const auto* targeted = targetedItem
        ? std::get_if<CombatItemCommand>(&targetedItem.command.payload) : nullptr;
    expect(targeted != nullptr
            && targeted->targetId == session.playerActorId(kGuestPlayerId),
        "targeted combat item retains its recipient identity");
    command.expectedPhase = SessionPhase::Combat;
    command.sequence.value = 9;
    command.payload = AttackCommand { session.playerActorId(kGuestPlayerId), 6, 8, 1 };
    expect(encodeGameCommand(command, envelope) == GameplayWireError::InvalidAttack,
        "reload hit mode cannot be smuggled through an attack command");

    GameEvent award;
    award.sequence = EventSequence { 42 };
    award.causedBy = CommandSequence { 1 };
    award.payload = PartyExperienceAwardedEvent { session.playerActorId(kHostPlayerId), 125, {
        { kHostPlayerId, session.playerActorId(kHostPlayerId), 250, 2, 12 },
        { kGuestPlayerId, session.playerActorId(kGuestPlayerId), 250, 2, 12 },
        { PlayerId { 3 }, EntityId { 99 }, 250, 2, 12 },
    } };
    expect(encodeGameEvent(award, envelope) == GameplayWireError::None,
        "roster-keyed shared XP event encodes");
    GameEventDecodeResult copiedAward = decodeGameEvent(envelope);
    const auto* progression = copiedAward
        ? std::get_if<PartyExperienceAwardedEvent>(&copiedAward.event.payload)
        : nullptr;
    expect(progression != nullptr && progression->amount == 125
            && progression->players.size() == 3
            && progression->players[2].playerId == PlayerId { 3 }
            && progression->players[2].level == 2,
        "shared XP and level progression round-trip for three players");
    std::get<PartyExperienceAwardedEvent>(award.payload).players[2].playerId = kGuestPlayerId;
    expect(encodeGameEvent(award, envelope) == GameplayWireError::InvalidCombatTurn,
        "shared XP event rejects duplicate participant results");
}

void testAuthoritativeCommandProcessing()
{
    TestObject hostActor;
    TestObject guestActor;
    TestObject door;
    TestObject item;
    TestObject lootableCritter;
    TestObject laterWorldObject;
    LocalSession session;
    expect(session.start(asGameObject(hostActor), asGameObject(guestActor)) == LocalSessionError::None, "command test session starts");
    submitBothCharacterSheets(session);
    expect(session.transitionTo(SessionPhase::Loading) == LocalSessionError::None, "command test session enters loading");
    expect(session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None, "command test session enters exploration");

    EntityRegistrationResult registeredDoor = session.registerWorldObject(asGameObject(door));
    expect(static_cast<bool>(registeredDoor), "door receives a world entity ID");
    expect(session.registerWorldObject(asGameObject(door)).entityId == registeredDoor.entityId, "registering the same world object returns its entity ID");
    EntityRegistrationResult registeredItem = session.registerWorldObject(asGameObject(item));
    EntityRegistrationResult registeredLootableCritter = session.registerWorldObject(asGameObject(lootableCritter));
    expect(static_cast<bool>(registeredItem) && static_cast<bool>(registeredLootableCritter), "item interaction targets receive world entity IDs");

    CommandProcessor processor;
    RecordingCommandExecutor executor;
    executor.modalSession = &session;

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
    expect(duplicate.replayed
            && duplicate.event.has_value()
            && duplicate.event->sequence == EventSequence { 1 },
        "duplicate command identifies the original event without republishing it");

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
    expect(doorEvent != nullptr
            && doorEvent->targetId == registeredDoor.entityId
            && doorEvent->open
            && !doorEvent->locked
            && doorEvent->frame == 3,
        "door event records authoritative state without requiring guest script execution");
    expect(usedDoor.event.has_value() && usedDoor.event->sequence == EventSequence { 2 }, "event sequence advances across players");

    CommandProcessor skillProcessor;
    GameCommand useSkill;
    useSkill.sequence.value = 1;
    useSkill.playerId = kGuestPlayerId;
    useSkill.actorId = session.playerActorId(kGuestPlayerId);
    useSkill.expectedPhase = SessionPhase::Exploration;
    useSkill.expectedPhaseRevision = session.phaseRevision();
    useSkill.payload = UseSkillCommand { registeredDoor.entityId, ExplorationSkill::Traps };
    AuthoritativeCommandResult usedSkill = skillProcessor.process(useSkill, session, executor);
    const SkillUseStartedEvent* skillEvent = usedSkill.event.has_value()
        ? std::get_if<SkillUseStartedEvent>(&usedSkill.event->payload)
        : nullptr;
    expect(usedSkill.result.status == CommandStatus::Accepted
            && executor.skillCalls == 1
            && executor.lastActor == asGameObject(guestActor)
            && executor.lastTarget == asGameObject(door)
            && executor.lastSkill.skill == ExplorationSkill::Traps
            && executor.lastActingPlayerId == kGuestPlayerId
            && skillEvent != nullptr
            && skillEvent->targetId == registeredDoor.entityId
            && skillEvent->skill == ExplorationSkill::Traps,
        "targeted skill executes once under the acting player's build and emits a replica-safe cue");
    useSkill.sequence.value = 2;
    std::get<UseSkillCommand>(useSkill.payload).skill = static_cast<ExplorationSkill>(99);
    AuthoritativeCommandResult invalidSkillUse = skillProcessor.process(useSkill, session, executor);
    expect(invalidSkillUse.result.rejection == CommandRejection::Malformed
            && executor.skillCalls == 1
            && !invalidSkillUse.event.has_value(),
        "command processor rejects unsupported skills before engine execution");

    CommandProcessor itemUseProcessor;
    GameCommand useItem;
    useItem.sequence.value = 1;
    useItem.playerId = kGuestPlayerId;
    useItem.actorId = session.playerActorId(kGuestPlayerId);
    useItem.expectedPhase = SessionPhase::Exploration;
    useItem.expectedPhaseRevision = session.phaseRevision();
    useItem.payload = UseItemOnCommand { registeredItem.entityId, registeredLootableCritter.entityId };
    AuthoritativeCommandResult usedItem = itemUseProcessor.process(useItem, session, executor);
    const ItemUseStartedEvent* itemUseEvent = usedItem.event.has_value()
        ? std::get_if<ItemUseStartedEvent>(&usedItem.event->payload)
        : nullptr;
    expect(usedItem.result.status == CommandStatus::Accepted
            && executor.itemUseCalls == 1
            && executor.lastActor == asGameObject(guestActor)
            && executor.lastSource == asGameObject(item)
            && executor.lastTarget == asGameObject(lootableCritter)
            && executor.lastActingPlayerId == kGuestPlayerId
            && itemUseEvent != nullptr
            && itemUseEvent->itemId == registeredItem.entityId
            && itemUseEvent->targetId == registeredLootableCritter.entityId,
        "host resolves and executes guest-owned item use under the acting-player context");
    useItem.sequence.value = 2;
    std::get<UseItemOnCommand>(useItem.payload).targetId = registeredItem.entityId;
    AuthoritativeCommandResult invalidItemUse = itemUseProcessor.process(useItem, session, executor);
    expect(invalidItemUse.result.rejection == CommandRejection::Malformed
            && executor.itemUseCalls == 1
            && !invalidItemUse.event.has_value(),
        "command processor rejects an item used on itself before engine execution");

    CommandProcessor elevatorProcessor;
    GameCommand useElevator;
    useElevator.sequence.value = 1;
    useElevator.playerId = kGuestPlayerId;
    useElevator.actorId = session.playerActorId(kGuestPlayerId);
    useElevator.expectedPhase = SessionPhase::Exploration;
    useElevator.expectedPhaseRevision = session.phaseRevision();
    useElevator.payload = ElevatorCommand { 8, 1 };
    AuthoritativeCommandResult usedElevator = elevatorProcessor.process(useElevator, session, executor);
    const ElevatorTransitionedEvent* elevatorEvent = usedElevator.event.has_value()
        ? std::get_if<ElevatorTransitionedEvent>(&usedElevator.event->payload)
        : nullptr;
    expect(usedElevator.result.status == CommandStatus::Accepted
            && executor.elevatorCalls == 1
            && executor.lastActor == asGameObject(guestActor)
            && executor.lastElevator.elevatorType == 8
            && executor.lastElevator.destinationLevel == 1
            && elevatorEvent != nullptr
            && elevatorEvent->actorId == session.playerActorId(kGuestPlayerId)
            && elevatorEvent->elevatorType == 8
            && elevatorEvent->map == 6
            && elevatorEvent->hostTile == 14105
            && elevatorEvent->hostElevation == 0
            && elevatorEvent->guestTile == 22504
            && elevatorEvent->guestElevation == 1
            && elevatorEvent->phaseRevision == 5,
        "host executes a validated elevator command and emits independent exact player placement state");
    useElevator.sequence.value = 2;
    std::get<ElevatorCommand>(useElevator.payload).elevatorType = 12;
    AuthoritativeCommandResult invalidElevator = elevatorProcessor.process(useElevator, session, executor);
    expect(invalidElevator.result.rejection == CommandRejection::Malformed
            && executor.elevatorCalls == 1
            && !invalidElevator.event.has_value(),
        "command processor rejects an unknown elevator table before engine execution");

    CommandProcessor exitGridProcessor;
    GameCommand useExitGrid;
    useExitGrid.sequence.value = 1;
    useExitGrid.playerId = kGuestPlayerId;
    useExitGrid.actorId = session.playerActorId(kGuestPlayerId);
    useExitGrid.expectedPhase = SessionPhase::Exploration;
    useExitGrid.expectedPhaseRevision = session.phaseRevision();
    useExitGrid.payload = ExitGridCommand { registeredLootableCritter.entityId };
    AuthoritativeCommandResult usedExitGrid = exitGridProcessor.process(useExitGrid, session, executor);
    const ExitGridTransitionedEvent* exitGridEvent = usedExitGrid.event.has_value()
        ? std::get_if<ExitGridTransitionedEvent>(&usedExitGrid.event->payload)
        : nullptr;
    expect(usedExitGrid.result.status == CommandStatus::Accepted
            && executor.exitGridCalls == 1
            && executor.lastActor == asGameObject(guestActor)
            && executor.lastTarget == asGameObject(lootableCritter)
            && executor.lastExitGrid.exitId == registeredLootableCritter.entityId
            && exitGridEvent != nullptr
            && exitGridEvent->actorId == session.playerActorId(kGuestPlayerId)
            && exitGridEvent->exitId == registeredLootableCritter.entityId
            && exitGridEvent->map == 35
            && exitGridEvent->placements.size() == 2
            && exitGridEvent->placements[1].playerId == kGuestPlayerId
            && exitGridEvent->phaseRevision == 7,
        "host resolves an exit-grid target and emits a player-keyed transition roster");
    useExitGrid.sequence.value = 2;
    std::get<ExitGridCommand>(useExitGrid.payload).exitId = {};
    AuthoritativeCommandResult invalidExitGrid = exitGridProcessor.process(useExitGrid, session, executor);
    expect(invalidExitGrid.result.rejection == CommandRejection::Malformed
            && executor.exitGridCalls == 1
            && !invalidExitGrid.event.has_value(),
        "command processor rejects an invalid exit-grid identity before engine execution");

    CommandProcessor sceneryTransitionProcessor;
    GameCommand useSceneryTransition;
    useSceneryTransition.sequence.value = 1;
    useSceneryTransition.playerId = kGuestPlayerId;
    useSceneryTransition.actorId = session.playerActorId(kGuestPlayerId);
    useSceneryTransition.expectedPhase = SessionPhase::Exploration;
    useSceneryTransition.expectedPhaseRevision = session.phaseRevision();
    useSceneryTransition.payload = SceneryTransitionCommand { registeredLootableCritter.entityId };
    AuthoritativeCommandResult usedSceneryTransition = sceneryTransitionProcessor.process(useSceneryTransition, session, executor);
    const SceneryTransitionedEvent* sceneryTransitionEvent = usedSceneryTransition.event.has_value()
        ? std::get_if<SceneryTransitionedEvent>(&usedSceneryTransition.event->payload)
        : nullptr;
    expect(usedSceneryTransition.result.status == CommandStatus::Accepted
            && executor.sceneryTransitionCalls == 1
            && executor.lastActor == asGameObject(guestActor)
            && executor.lastTarget == asGameObject(lootableCritter)
            && executor.lastSceneryTransition.transitionId == registeredLootableCritter.entityId
            && sceneryTransitionEvent != nullptr
            && sceneryTransitionEvent->map == 5
            && sceneryTransitionEvent->placements.size() == 2
            && sceneryTransitionEvent->placements[1].elevation == 1
            && sceneryTransitionEvent->phaseRevision == 9,
        "host resolves scenery-transition targets and emits player-keyed placements");

    CommandProcessor restProcessor;
    GameCommand requestRest;
    requestRest.sequence.value = 1;
    requestRest.playerId = kGuestPlayerId;
    requestRest.actorId = session.playerActorId(kGuestPlayerId);
    requestRest.expectedPhase = SessionPhase::Exploration;
    requestRest.expectedPhaseRevision = session.phaseRevision();
    requestRest.payload = RestCommand { 180 };
    AuthoritativeCommandResult requestedRest = restProcessor.process(requestRest, session, executor);
    const RestStateChangedEvent* restEvent = requestedRest.event.has_value()
        ? std::get_if<RestStateChangedEvent>(&requestedRest.event->payload)
        : nullptr;
    expect(requestedRest.result.status == CommandStatus::Accepted
            && executor.restCalls == 1
            && executor.lastRest.minutes == 180
            && executor.lastActingPlayerId == kGuestPlayerId
            && restEvent != nullptr
            && restEvent->completed
            && restEvent->gameTime == 231456,
        "rest consent routes through the acting-player context and emits authoritative completion");
    requestRest.sequence.value = 2;
    std::get<RestCommand>(requestRest.payload).minutes = 17;
    AuthoritativeCommandResult invalidRest = restProcessor.process(requestRest, session, executor);
    expect(invalidRest.result.rejection == CommandRejection::Malformed
            && executor.restCalls == 1,
        "rest rejects unsupported durations before invoking the engine");

    GameCommand pickup;
    pickup.sequence.value = 2;
    pickup.playerId = kHostPlayerId;
    pickup.actorId = session.playerActorId(kHostPlayerId);
    pickup.expectedPhase = SessionPhase::Exploration;
    pickup.expectedPhaseRevision = session.phaseRevision();
    pickup.payload = PickupCommand { registeredItem.entityId };
    executor.reservePickups = true;
    AuthoritativeCommandResult pickedUp = processor.process(pickup, session, executor);
    expect(pickedUp.result.status == CommandStatus::Accepted, "owned pickup command is accepted");
    expect(executor.pickupCalls == 1 && executor.lastActor == asGameObject(hostActor) && executor.lastTarget == asGameObject(item), "pickup resolves the owned actor and authoritative item");
    const ItemPickupStartedEvent* pickupEvent = pickedUp.event.has_value() ? std::get_if<ItemPickupStartedEvent>(&pickedUp.event->payload) : nullptr;
    expect(pickupEvent != nullptr && pickupEvent->actorId == session.playerActorId(kHostPlayerId) && pickupEvent->targetId == registeredItem.entityId, "pickup event records the actor and item IDs");

    GameCommand contestedPickup = pickup;
    contestedPickup.sequence.value = 3;
    contestedPickup.playerId = kGuestPlayerId;
    contestedPickup.actorId = session.playerActorId(kGuestPlayerId);
    AuthoritativeCommandResult contested = processor.process(contestedPickup, session, executor);
    expect(contested.result.status == CommandStatus::Rejected
            && contested.result.rejection == CommandRejection::InvalidAction
            && !contested.event.has_value()
            && executor.pickupCalls == 2,
        "a second actor cannot claim an item reserved by an in-flight pickup");

    GameCommand loot;
    loot.sequence.value = 4;
    loot.playerId = kGuestPlayerId;
    loot.actorId = session.playerActorId(kGuestPlayerId);
    loot.expectedPhase = SessionPhase::Exploration;
    loot.expectedPhaseRevision = session.phaseRevision();
    loot.payload = LootCommand { registeredLootableCritter.entityId };
    AuthoritativeCommandResult looted = processor.process(loot, session, executor);
    expect(looted.result.status == CommandStatus::Accepted, "owned loot command is accepted");
    expect(executor.lootCalls == 1 && executor.lastActor == asGameObject(guestActor) && executor.lastTarget == asGameObject(lootableCritter), "loot resolves the guest actor and authoritative critter");
    expect(executor.lastActingPlayerId == kGuestPlayerId && executor.lastBuild == &session.players().find(kGuestPlayerId)->build, "loot executes with guest character rules");
    const LootStartedEvent* lootEvent = looted.event.has_value() ? std::get_if<LootStartedEvent>(&looted.event->payload) : nullptr;
    expect(lootEvent != nullptr && lootEvent->actorId == session.playerActorId(kGuestPlayerId) && lootEvent->targetId == registeredLootableCritter.entityId, "loot event records the actor and target IDs");
    expect(looted.event.has_value() && looted.event->sequence == EventSequence { 4 }, "item interactions share the authoritative event sequence");

    GameCommand transfer;
    transfer.sequence.value = 5;
    transfer.playerId = kGuestPlayerId;
    transfer.actorId = session.playerActorId(kGuestPlayerId);
    transfer.expectedPhase = SessionPhase::Exploration;
    transfer.expectedPhaseRevision = session.phaseRevision();
    transfer.payload = InventoryTransferCommand {
        registeredLootableCritter.entityId,
        session.playerActorId(kGuestPlayerId),
        registeredItem.entityId,
        2,
        2,
    };
    AuthoritativeCommandResult transferred = processor.process(transfer, session, executor);
    const InventoryTransferredEvent* transferEvent = transferred.event.has_value()
        ? std::get_if<InventoryTransferredEvent>(&transferred.event->payload)
        : nullptr;
    expect(transferred.result.status == CommandStatus::Accepted
            && executor.transferCalls == 1
            && executor.lastSource == asGameObject(lootableCritter)
            && executor.lastDestination == asGameObject(guestActor)
            && executor.lastTarget == asGameObject(item)
            && executor.lastQuantity == 2,
        "host resolves and executes an owned guest inventory transfer");
    expect(transferEvent != nullptr
            && transferEvent->actorId == session.playerActorId(kGuestPlayerId)
            && transferEvent->sourceId == registeredLootableCritter.entityId
            && transferEvent->destinationId == session.playerActorId(kGuestPlayerId)
            && transferEvent->itemId == registeredItem.entityId
            && transferEvent->quantity == 2
            && transferEvent->sourceQuantity == 2,
        "accepted inventory mutation emits a complete authoritative transfer event");

    transfer.sequence.value = 6;
    transfer.payload = InventoryTransferCommand {
        registeredLootableCritter.entityId,
        session.playerActorId(kGuestPlayerId),
        {},
        1,
        4,
        ItemDescriptor { 40, 0, 0, 0 },
    };
    AuthoritativeCommandResult dynamicTransferred = processor.process(transfer, session, executor);
    const InventoryTransferredEvent* dynamicTransferEvent = dynamicTransferred.event.has_value()
        ? std::get_if<InventoryTransferredEvent>(&dynamicTransferred.event->payload)
        : nullptr;
    expect(dynamicTransferred.result.status == CommandStatus::Accepted
            && executor.lastTarget == nullptr
            && dynamicTransferEvent != nullptr
            && dynamicTransferEvent->itemId == EntityId { 88 }
            && dynamicTransferEvent->remainderItemId == EntityId { 89 }
            && dynamicTransferEvent->sourceQuantity == 4
            && dynamicTransferEvent->itemDescriptor.pid == 40,
        "host execution assigns identities to a dynamic item and its split remainder");

    GameCommand drop;
    drop.sequence.value = 7;
    drop.playerId = kGuestPlayerId;
    drop.actorId = session.playerActorId(kGuestPlayerId);
    drop.expectedPhase = SessionPhase::Exploration;
    drop.expectedPhaseRevision = session.phaseRevision();
    drop.payload = ItemDropCommand {
        session.playerActorId(kGuestPlayerId),
        registeredItem.entityId,
        1,
        3,
        ItemDescriptor { 40, 0, 0, 0 },
    };
    AuthoritativeCommandResult dropped = processor.process(drop, session, executor);
    const ItemDroppedEvent* dropEvent = dropped.event.has_value()
        ? std::get_if<ItemDroppedEvent>(&dropped.event->payload)
        : nullptr;
    expect(dropped.result.status == CommandStatus::Accepted
            && executor.dropCalls == 1
            && executor.lastActor == asGameObject(guestActor)
            && executor.lastSource == asGameObject(guestActor)
            && executor.lastTarget == asGameObject(item)
            && dropEvent != nullptr
            && dropEvent->itemId == registeredItem.entityId
            && dropEvent->remainderItemId == EntityId { 91 }
            && dropEvent->tile == 12345
            && dropEvent->elevation == 1,
        "host execution emits authoritative identity and placement for a dropped stack item");

    GameCommand attack;
    attack.sequence.value = 8;
    attack.playerId = kGuestPlayerId;
    attack.actorId = session.playerActorId(kGuestPlayerId);
    expect(session.transitionTo(SessionPhase::Combat) == LocalSessionError::None,
        "command test enters combat before accepting an attack");
    attack.expectedPhase = SessionPhase::Combat;
    attack.expectedPhaseRevision = session.phaseRevision();
    attack.payload = AttackCommand { registeredLootableCritter.entityId, 1, 8, 1 };
    AuthoritativeCommandResult attacked = processor.process(attack, session, executor);
    const AttackStartedEvent* attackEvent = attacked.event.has_value()
        ? std::get_if<AttackStartedEvent>(&attacked.event->payload)
        : nullptr;
    expect(attacked.result.status == CommandStatus::Accepted
            && executor.attackCalls == 1
            && executor.lastActor == asGameObject(guestActor)
            && executor.lastTarget == asGameObject(lootableCritter)
            && executor.lastAttack.hitMode == 1
            && attackEvent != nullptr
            && attackEvent->targetId == registeredLootableCritter.entityId,
        "host resolves and executes a guest attack against the shared NPC identity");

    expect(session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None,
        "command test returns to exploration after the attack");

    GameCommand openModal;
    openModal.sequence.value = 9;
    openModal.playerId = kGuestPlayerId;
    openModal.actorId = session.playerActorId(kGuestPlayerId);
    openModal.expectedPhase = SessionPhase::Exploration;
    openModal.expectedPhaseRevision = session.phaseRevision();
    openModal.payload = SharedModalCommand { SharedModalKind::Dialogue, true };
    AuthoritativeCommandResult openedModal = processor.process(openModal, session, executor);
    const SharedModalStateChangedEvent* openedModalEvent = openedModal.event.has_value()
        ? std::get_if<SharedModalStateChangedEvent>(&openedModal.event->payload)
        : nullptr;
    expect(openedModal.result.status == CommandStatus::Accepted
            && executor.modalCalls == 1
            && session.phase() == SessionPhase::Dialogue
            && openedModalEvent != nullptr
            && openedModalEvent->open
            && openedModalEvent->phase == SessionPhase::Dialogue
            && openedModalEvent->phaseRevision == session.phaseRevision(),
        "shared modal open publishes the authoritative dialogue phase boundary");

    GameCommand pausedMove = openModal;
    pausedMove.sequence.value = 10;
    pausedMove.expectedPhase = SessionPhase::Dialogue;
    pausedMove.expectedPhaseRevision = session.phaseRevision();
    pausedMove.payload = MoveCommand { 100, 0, false };
    AuthoritativeCommandResult pausedMovement = processor.process(pausedMove, session, executor);
    expect(pausedMovement.result.rejection == CommandRejection::WrongPhase
            && !pausedMovement.event.has_value(),
        "ordinary exploration commands are rejected while a shared modal pauses the world");

    GameCommand closeModal = openModal;
    closeModal.sequence.value = 11;
    closeModal.expectedPhase = SessionPhase::Dialogue;
    closeModal.expectedPhaseRevision = session.phaseRevision();
    closeModal.payload = SharedModalCommand { SharedModalKind::Dialogue, false };
    AuthoritativeCommandResult closedModal = processor.process(closeModal, session, executor);
    const SharedModalStateChangedEvent* closedModalEvent = closedModal.event.has_value()
        ? std::get_if<SharedModalStateChangedEvent>(&closedModal.event->payload)
        : nullptr;
    expect(closedModal.result.status == CommandStatus::Accepted
            && executor.modalCalls == 2
            && session.phase() == SessionPhase::Exploration
            && closedModalEvent != nullptr
            && !closedModalEvent->open
            && closedModalEvent->phase == SessionPhase::Exploration
            && closedModalEvent->phaseRevision == session.phaseRevision(),
        "shared modal close resumes exploration at a new authoritative phase revision");

    executor.nextStatus = CommandExecutionStatus::InvalidAction;
    move.sequence.value = 3;
    move.expectedPhase = SessionPhase::Exploration;
    move.expectedPhaseRevision = session.phaseRevision();
    AuthoritativeCommandResult invalid = processor.process(move, session, executor);
    expect(invalid.result.rejection == CommandRejection::InvalidAction, "executor can reject an impossible action");
    expect(!invalid.event.has_value(), "rejected action emits no authoritative event");
    expect(actingPlayerState() == nullptr, "invalid action does not leak its acting-player context");

    move.sequence.value = 5;
    AuthoritativeCommandResult gap = processor.process(move, session, executor);
    expect(gap.result.rejection == CommandRejection::Stale, "command sequence gap is rejected");
    expect(executor.moveCalls == 2, "sequence gap does not reach the executor");

    executor.nextStatus = CommandExecutionStatus::Applied;
    move.sequence.value = 4;
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
    move.sequence.value = 5;
    move.expectedPhase = SessionPhase::Combat;
    move.expectedPhaseRevision = session.phaseRevision();
    move.payload = MoveCommand { 100, 0, false };
    AuthoritativeCommandResult wrongPhase = processor.process(move, session, executor);
    expect(wrongPhase.result.rejection == CommandRejection::WrongPhase, "exploration command is rejected during combat");

    std::uint32_t combatRevision = session.phaseRevision();
    expect(session.transitionTo(SessionPhase::Exploration) == LocalSessionError::None, "command test session returns to exploration");
    move.sequence.value = 6;
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
    authoritative.nextExtraCapPlayer = kGuestPlayerId;
    authoritative.nextItemPriorityPlayer = kGuestPlayerId;
    std::vector<std::uint8_t> packet;
    expect(encodeSnapshot(authoritative, packet) == SnapshotError::None, "valid snapshot encodes");
    constexpr std::size_t characterBuildWireSize = (SAVEABLE_STAT_COUNT * 2
                                                        + SKILL_COUNT
                                                        + PERK_COUNT
                                                        + NUM_TAGGED_SKILLS
                                                        + PC_TRAIT_MAX
                                                        + 4)
        * sizeof(std::uint32_t);
    expect(packet.size() == kSnapshotHeaderSize + 48 + 2 * (68 + characterBuildWireSize) + 68 + 12 + 48 + 2 * 56 + 6 * 4 + 2 * 36 + 31 * 29 + 15 * 7 + 6 * 4 + 20 + 14 * 4 + 32 + 8 + 4 + 8,
        "snapshot packet declares a fixed-width payload");
    expect(packet[0] == 'F' && packet[1] == 'C' && packet[2] == 'M' && packet[3] == 'S', "snapshot magic uses network byte order");

    SnapshotDecodeResult decoded = decodeSnapshot(packet);
    expect(static_cast<bool>(decoded), "encoded snapshot decodes");
    expect(decoded.snapshot.lastIncludedEvent == EventSequence { 41 }, "snapshot keeps the last included event");
    expect(decoded.snapshot.phase == SessionPhase::Exploration
            && decoded.snapshot.phaseRevision == 7
            && decoded.snapshot.gameTime == 302400
            && decoded.snapshot.nextExtraCapPlayer == kGuestPlayerId
            && decoded.snapshot.nextItemPriorityPlayer == kGuestPlayerId,
        "snapshot keeps session phase and authoritative world time");
    expect(decoded.snapshot.worldMap.specialEncounters == 2
            && decoded.snapshot.worldMap.x == 1075
            && decoded.snapshot.worldMap.grid[42] == 2
            && decoded.snapshot.worldMap.knownTownEntrances[14] == 1,
        "snapshot keeps authoritative world-map discovery and encounter history");
    expect(decoded.snapshot.actors.size() == 2 && decoded.snapshot.actors[0].entityId == EntityId { 1 }, "decoded actors use canonical entity order");
    expect(decoded.snapshot.actors[1].tile == 20102
            && decoded.snapshot.actors[1].hitPoints == 28
            && decoded.snapshot.actors[1].fid == 0x01000002
            && decoded.snapshot.actors[1].frame == 2
            && decoded.snapshot.actors[1].whoHitMeId == EntityId { 12 },
        "snapshot keeps guest actor combat and presentation state");
    expect(decoded.snapshot.actors[0].build.experience == 125
            && decoded.snapshot.actors[1].build.experience == 2500
            && decoded.snapshot.actors[1].build.level == 2
            && decoded.snapshot.actors[1].build.unspentSkillPoints == 7
            && (decoded.snapshot.actors[1].build.prototypeFlags & (1 << 3)) != 0,
        "snapshot keeps each player's complete progressing character build");
    expect(decoded.snapshot.critters.size() == 1
            && decoded.snapshot.critters[0].entityId == EntityId { 12 }
            && decoded.snapshot.critters[0].hitPoints == 6
            && decoded.snapshot.critters[0].actionPoints == 7
            && decoded.snapshot.critters[0].fid == 0x01000004
            && decoded.snapshot.critters[0].frame == 3
            && decoded.snapshot.critters[0].damageLastTurn == 5
            && decoded.snapshot.critters[0].whoHitMeId == EntityId { 2 },
        "snapshot keeps authoritative NPC combat state");
    expect(decoded.snapshot.doors.size() == 1 && decoded.snapshot.doors[0].open, "snapshot keeps door state");
    expect(decoded.snapshot.scenery.size() == 1
            && decoded.snapshot.scenery[0].entityId == EntityId { 13 }
            && decoded.snapshot.scenery[0].pid == 0x02000001
            && decoded.snapshot.scenery[0].fid == 0x02000002
            && decoded.snapshot.scenery[0].frame == 1
            && decoded.snapshot.scenery[0].data1 == 8,
        "snapshot keeps concrete non-door scenery state");
    expect(decoded.snapshot.items.size() == 2
            && decoded.snapshot.items[0].holderId == EntityId { 1 }
            && decoded.snapshot.items[0].quantity == 2
            && decoded.snapshot.items[0].itemDescriptor.pid == 40
            && decoded.snapshot.items[1].tile == 20104,
        "snapshot keeps canonical inventory ownership and ground-item location");
    expect(decoded.snapshot.gameGlobalVariables == std::vector<std::int32_t>({ 7, -9 })
            && decoded.snapshot.mapGlobalVariables == std::vector<std::int32_t>({ 11 })
            && decoded.snapshot.mapLocalVariables == std::vector<std::int32_t>({ 100, 101, 102 }),
        "snapshot keeps game globals and indexed map script variables");
    expect(decoded.snapshot.timedEvents.size() == 2
            && decoded.snapshot.timedEvents[0].eventType == 3
            && decoded.snapshot.timedEvents[0].payload[0] == 0x01000042
            && decoded.snapshot.timedEvents[0].payload[1] == -7
            && decoded.snapshot.timedEvents[1].eventType == 5
            && decoded.snapshot.timedEvents[1].ownerId == EntityId { 1 },
        "snapshot keeps ordered timed events, owners, and type-specific payloads");

    SnapshotDigestResult authoritativeDigest = computeSnapshotDigest(authoritative);
    SnapshotDigestResult decodedDigest = computeSnapshotDigest(decoded.snapshot);
    expect(static_cast<bool>(authoritativeDigest) && authoritativeDigest.digest == decodedDigest.digest, "snapshot digest is stable across wire round trip and input order");
    expect(firstDivergentSection(authoritativeDigest.digest, decodedDigest.digest) == SnapshotSection::None, "matching snapshots report no divergent section");
    WorldSnapshot lootCursorDrift = decoded.snapshot;
    lootCursorDrift.nextExtraCapPlayer = kHostPlayerId;
    expect(firstDivergentSection(authoritativeDigest.digest,
               computeSnapshotDigest(lootCursorDrift).digest) == SnapshotSection::Session,
        "loot cursor drift reports the session section");
    WorldSnapshot invalidLootCursor = decoded.snapshot;
    invalidLootCursor.nextItemPriorityPlayer = PlayerId { 3 };
    expect(validateSnapshot(invalidLootCursor) == SnapshotError::InvalidPlayerId,
        "snapshot rejects a loot cursor outside its player roster");

    WorldSnapshot sessionDrift = decoded.snapshot;
    sessionDrift.phaseRevision++;
    SnapshotDigestResult sessionDriftDigest = computeSnapshotDigest(sessionDrift);
    expect(firstDivergentSection(authoritativeDigest.digest, sessionDriftDigest.digest) == SnapshotSection::Session, "phase drift reports the session section first");

    WorldSnapshot timeDrift = decoded.snapshot;
    timeDrift.gameTime++;
    SnapshotDigestResult timeDriftDigest = computeSnapshotDigest(timeDrift);
    expect(firstDivergentSection(authoritativeDigest.digest, timeDriftDigest.digest) == SnapshotSection::Session,
        "world-time drift reports the session section");

    WorldSnapshot actorDrift = decoded.snapshot;
    actorDrift.actors[1].tile++;
    SnapshotDigestResult actorDriftDigest = computeSnapshotDigest(actorDrift);
    expect(firstDivergentSection(authoritativeDigest.digest, actorDriftDigest.digest) == SnapshotSection::Actors, "position drift reports the actor section");

    WorldSnapshot buildDrift = decoded.snapshot;
    buildDrift.actors[1].build.experience++;
    SnapshotDigestResult buildDriftDigest = computeSnapshotDigest(buildDrift);
    expect(firstDivergentSection(authoritativeDigest.digest, buildDriftDigest.digest) == SnapshotSection::Actors,
        "character-build drift reports the owning actor section");

    WorldSnapshot critterDrift = decoded.snapshot;
    critterDrift.critters[0].hitPoints--;
    SnapshotDigestResult critterDriftDigest = computeSnapshotDigest(critterDrift);
    expect(firstDivergentSection(authoritativeDigest.digest, critterDriftDigest.digest) == SnapshotSection::Critters,
        "NPC combat drift reports the critter section");

    WorldSnapshot doorDrift = decoded.snapshot;
    doorDrift.doors[0].open = false;
    SnapshotDigestResult doorDriftDigest = computeSnapshotDigest(doorDrift);
    expect(firstDivergentSection(authoritativeDigest.digest, doorDriftDigest.digest) == SnapshotSection::Doors, "door drift reports the door section");

    WorldSnapshot sceneryDrift = decoded.snapshot;
    sceneryDrift.scenery[0].frame++;
    SnapshotDigestResult sceneryDriftDigest = computeSnapshotDigest(sceneryDrift);
    expect(firstDivergentSection(authoritativeDigest.digest, sceneryDriftDigest.digest) == SnapshotSection::Scenery,
        "non-door scenery drift reports the scenery section");

    WorldSnapshot itemDrift = decoded.snapshot;
    itemDrift.items[0].frame++;
    SnapshotDigestResult itemDriftDigest = computeSnapshotDigest(itemDrift);
    expect(firstDivergentSection(authoritativeDigest.digest, itemDriftDigest.digest) == SnapshotSection::Items,
        "item presentation drift reports the item section");

    WorldSnapshot globalDrift = decoded.snapshot;
    globalDrift.gameGlobalVariables[0]++;
    SnapshotDigestResult globalDriftDigest = computeSnapshotDigest(globalDrift);
    expect(firstDivergentSection(authoritativeDigest.digest, globalDriftDigest.digest) == SnapshotSection::Globals,
        "game-global drift reports the globals section");

    WorldSnapshot mapVariableDrift = decoded.snapshot;
    mapVariableDrift.mapLocalVariables[1]++;
    SnapshotDigestResult mapVariableDriftDigest = computeSnapshotDigest(mapVariableDrift);
    expect(firstDivergentSection(authoritativeDigest.digest, mapVariableDriftDigest.digest) == SnapshotSection::MapVariables,
        "map script-variable drift reports the map-variable section");

    WorldSnapshot timedEventDrift = decoded.snapshot;
    timedEventDrift.timedEvents[0].payload[1]++;
    SnapshotDigestResult timedEventDriftDigest = computeSnapshotDigest(timedEventDrift);
    expect(firstDivergentSection(authoritativeDigest.digest, timedEventDriftDigest.digest) == SnapshotSection::TimedEvents,
        "timed-event drift reports the timed-event section");

    WorldSnapshot worldMapDrift = decoded.snapshot;
    worldMapDrift.worldMap.specialEncounters ^= 1;
    SnapshotDigestResult worldMapDriftDigest = computeSnapshotDigest(worldMapDrift);
    expect(firstDivergentSection(authoritativeDigest.digest, worldMapDriftDigest.digest) == SnapshotSection::WorldMap,
        "encounter-history drift reports the world-map section");
    WorldSnapshot invalidWorldMap = decoded.snapshot;
    invalidWorldMap.worldMap.grid[42] = 3;
    expect(validateSnapshot(invalidWorldMap) == SnapshotError::InvalidWorldMapState,
        "snapshot rejects an invalid world-map discovery cell");
    WorldSnapshot proposedTravel = decoded.snapshot;
    proposedTravel.worldMapTravel = WorldMapTravelSnapshot {
        EntityId { 2 }, EntityId { 2 }, WorldMapTravelStage::Proposed, -1, -1
    };
    std::vector<std::uint8_t> planningPacket;
    expect(encodeSnapshot(proposedTravel, planningPacket) == SnapshotError::None
            && decodeSnapshot(planningPacket).snapshot.worldMapTravel.proposerActorId == EntityId { 2 },
        "snapshot recovers a pending guest-proposed world-map trip");
    WorldSnapshot approvedTravel = proposedTravel;
    approvedTravel.phase = SessionPhase::Transition;
    approvedTravel.worldMapTravel.stage = WorldMapTravelStage::Approved;
    approvedTravel.worldMapTravel.targetX = 512;
    approvedTravel.worldMapTravel.targetY = 824;
    expect(encodeSnapshot(approvedTravel, planningPacket) == SnapshotError::None
            && decodeSnapshot(planningPacket).snapshot.worldMapTravel.targetY == 824,
        "snapshot recovers the approved controller and selected route");
    approvedTravel.worldMapTravel.controllerActorId = EntityId { 1 };
    expect(encodeSnapshot(approvedTravel, planningPacket) == SnapshotError::None
            && decodeSnapshot(planningPacket).snapshot.worldMapTravel.controllerActorId == EntityId { 1 }
            && decodeSnapshot(planningPacket).snapshot.worldMapTravel.proposerActorId == EntityId { 2 },
        "snapshot preserves a guest proposal after host takeover");
    WorldSnapshot routeDrift = approvedTravel;
    routeDrift.worldMapTravel.targetX = 513;
    SnapshotDigestResult travelDigest = computeSnapshotDigest(approvedTravel);
    SnapshotDigestResult routeDriftDigest = computeSnapshotDigest(routeDrift);
    expect(firstDivergentSection(travelDigest.digest, routeDriftDigest.digest) == SnapshotSection::WorldMap,
        "travel-controller drift reports the world-map section");
    approvedTravel.worldMapTravel.progress = WorldMapTravelProgress {
        true, 512, 824, 563, 749, 0, 1, -1, 1, 2, 1, 1, 80, 10800
    };
    expect(encodeSnapshot(approvedTravel, planningPacket) == SnapshotError::None
            && decodeSnapshot(planningPacket).snapshot.worldMapTravel.progress.active
            && decodeSnapshot(planningPacket).snapshot.worldMapTravel.progress.lineIndex == 1,
        "snapshot recovers an active world-map travel line");
    WorldSnapshot travelCounterDrift = approvedTravel;
    travelCounterDrift.worldMapTravel.progress.lineIndex = 2;
    SnapshotDigestResult travelCounterDigest = computeSnapshotDigest(travelCounterDrift);
    expect(firstDivergentSection(computeSnapshotDigest(approvedTravel).digest,
               travelCounterDigest.digest) == SnapshotSection::WorldMap,
        "travel-counter drift reports the world-map section");
    WorldSnapshot invalidTravelCounter = approvedTravel;
    invalidTravelCounter.worldMapTravel.progress.lineIndex = 1500;
    expect(validateSnapshot(invalidTravelCounter) == SnapshotError::InvalidWorldMapTravelState,
        "snapshot rejects an impossible travel line index");
    WorldSnapshot mismatchedTravelTarget = approvedTravel;
    mismatchedTravelTarget.worldMapTravel.progress.targetX = 513;
    expect(validateSnapshot(mismatchedTravelTarget) == SnapshotError::InvalidWorldMapTravelState,
        "snapshot rejects a travel line for a different selected route");
    approvedTravel.worldMapTravel.targetX = kWorldMapWidth;
    expect(validateSnapshot(approvedTravel) == SnapshotError::InvalidWorldMapTravelState,
        "snapshot rejects an out-of-bounds travel target");

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
    invalidOwner = authoritative;
    invalidOwner.actors[0].ownerId = invalidOwner.actors[1].ownerId;
    expect(validateSnapshot(invalidOwner) == SnapshotError::InvalidPlayerId,
        "two-player snapshot rejects duplicate player ownership");

    WorldSnapshot invalidBuild = authoritative;
    invalidBuild.actors[0].build.level = 0;
    expect(validateSnapshot(invalidBuild) == SnapshotError::InvalidPlayerBuild,
        "snapshot rejects an invalid player level");
    invalidBuild = authoritative;
    invalidBuild.actors[0].build.taggedSkills = { SKILL_SPEECH, SKILL_SPEECH, -1, -1 };
    expect(validateSnapshot(invalidBuild) == SnapshotError::InvalidPlayerBuild,
        "snapshot rejects duplicate tagged skills in a player build");

    WorldSnapshot invalidGameTime = authoritative;
    invalidGameTime.gameTime = 0;
    expect(validateSnapshot(invalidGameTime) == SnapshotError::InvalidGameTime,
        "snapshot rejects an invalid authoritative world time");

    WorldSnapshot invalidItem = authoritative;
    invalidItem.items[0].quantity = 0;
    expect(validateSnapshot(invalidItem) == SnapshotError::InvalidItemState,
        "snapshot rejects an empty inventory stack");
    invalidItem = authoritative;
    invalidItem.items[0].holderId = invalidItem.items[0].entityId;
    expect(validateSnapshot(invalidItem) == SnapshotError::InvalidItemState,
        "snapshot rejects an item that contains itself");
    invalidItem = authoritative;
    invalidItem.items[1].quantity = 2;
    expect(validateSnapshot(invalidItem) == SnapshotError::InvalidItemState,
        "snapshot rejects an impossible ground stack");
    invalidItem = authoritative;
    invalidItem.items[0].itemDescriptor = {};
    expect(validateSnapshot(invalidItem) == SnapshotError::InvalidItemState,
        "snapshot requires enough item description to recreate a missing entity");
    invalidItem = authoritative;
    invalidItem.items[0].objectFlags = 0x40000000;
    expect(validateSnapshot(invalidItem) == SnapshotError::InvalidItemState,
        "snapshot rejects local-only item visibility state");

    WorldSnapshot invalidScenery = authoritative;
    invalidScenery.scenery[0].objectFlags = 0x40000000;
    expect(validateSnapshot(invalidScenery) == SnapshotError::InvalidSceneryState,
        "snapshot rejects local-only scenery visibility state");
    invalidScenery = authoritative;
    invalidScenery.scenery[0].pid = 0x01000001;
    expect(validateSnapshot(invalidScenery) == SnapshotError::InvalidSceneryState,
        "snapshot rejects a non-scenery prototype in the scenery section");

    WorldSnapshot tooManyVariables = authoritative;
    tooManyVariables.mapLocalVariables.assign(kMaxSnapshotVariables + 1, 0);
    expect(validateSnapshot(tooManyVariables) == SnapshotError::TooManyVariables,
        "snapshot bounds each script-visible variable array");

    WorldSnapshot invalidTimedEvent = authoritative;
    invalidTimedEvent.timedEvents[0].payloadCount = 1;
    expect(validateSnapshot(invalidTimedEvent) == SnapshotError::InvalidTimedEventState,
        "snapshot rejects a timed event with the wrong type-specific payload shape");
    invalidTimedEvent = authoritative;
    invalidTimedEvent.timedEvents[1].ownerId = {};
    expect(validateSnapshot(invalidTimedEvent) == SnapshotError::InvalidTimedEventState,
        "snapshot rejects an owner-dependent timed event without an entity");
    invalidTimedEvent = authoritative;
    std::swap(invalidTimedEvent.timedEvents[0].time, invalidTimedEvent.timedEvents[1].time);
    expect(validateSnapshot(invalidTimedEvent) == SnapshotError::InvalidTimedEventState,
        "snapshot rejects timed events outside queue order");

    WorldSnapshot tooManyTimedEvents = authoritative;
    tooManyTimedEvents.timedEvents.assign(kMaxSnapshotTimedEvents + 1, TimedEventSnapshot {});
    expect(validateSnapshot(tooManyTimedEvents) == SnapshotError::TooManyTimedEvents,
        "snapshot bounds the timed-event queue");
}

GameEvent sampleMovementEvent(std::uint64_t sequence)
{
    GameEvent event;
    event.sequence.value = sequence;
    event.causedBy.value = sequence;
    event.payload = ActorMovementStartedEvent { EntityId { 2 }, 20100 + static_cast<int>(sequence), 0, false };
    return event;
}

void testNetworkSessionRecoveryPrimitives()
{
    EventJournal journal(3, kDefaultEventJournalMaximumBytes);
    expect(journal.append(sampleMovementEvent(1)) == EventJournalError::None, "event journal accepts its first authoritative event");
    expect(journal.append(sampleMovementEvent(2)) == EventJournalError::None, "event journal accepts a contiguous event");
    expect(journal.append(sampleMovementEvent(4)) == EventJournalError::SequenceGap, "event journal rejects a sequence gap");
    expect(journal.latestSequence() == EventSequence { 2 }, "rejected journal append does not advance the sequence");

    EventReplay fromBeginning = journal.replayAfter(EventSequence {});
    expect(fromBeginning.status == EventReplayStatus::Available
            && fromBeginning.events.size() == 2
            && fromBeginning.events.front().sequence == EventSequence { 1 },
        "event journal replays retained events in order");
    expect(journal.replayAfter(EventSequence { 2 }).status == EventReplayStatus::UpToDate,
        "event journal recognizes an up-to-date peer");
    expect(journal.replayAfter(EventSequence { 3 }).status == EventReplayStatus::InvalidFutureSequence,
        "event journal rejects a peer sequence ahead of the host");

    expect(journal.append(sampleMovementEvent(3)) == EventJournalError::None, "event journal accepts event three");
    expect(journal.append(sampleMovementEvent(4)) == EventJournalError::None, "event journal accepts event four and evicts its oldest event");
    expect(journal.size() == 3 && journal.oldestSequence() == EventSequence { 2 },
        "event journal enforces its event-count bound");
    expect(journal.replayAfter(EventSequence {}).status == EventReplayStatus::SnapshotRequired,
        "a peer older than retained history requires a snapshot");
    EventReplay retainedReplay = journal.replayAfter(EventSequence { 1 });
    expect(retainedReplay.status == EventReplayStatus::Available
            && retainedReplay.events.size() == 3
            && retainedReplay.events.front().sequence == EventSequence { 2 },
        "a peer at the retention boundary receives journal replay");

    EventJournal byteBoundedJournal(10, 1);
    expect(byteBoundedJournal.append(sampleMovementEvent(1)) == EventJournalError::EventTooLarge,
        "event journal rejects an event larger than its byte budget");
    EventJournal invalidJournal(0, 100);
    expect(invalidJournal.append(sampleMovementEvent(1)) == EventJournalError::InvalidLimit,
        "event journal rejects a zero retention limit");
    journal.reset(EventSequence { 41 });
    expect(journal.empty()
            && journal.latestSequence() == EventSequence { 41 }
            && journal.replayAfter(EventSequence { 40 }).status == EventReplayStatus::SnapshotRequired
            && journal.append(sampleMovementEvent(42)) == EventJournalError::None,
        "event journal can resume after a restored snapshot without claiming evicted history");

    SessionId sessionId { 0x1020304050607080ULL };
    ReconnectToken token;
    for (std::size_t index = 0; index < token.bytes.size(); index++) {
        token.bytes[index] = static_cast<std::uint8_t>(index + 1);
    }
    ReconnectToken wrongToken = token;
    wrongToken.bytes.back() ^= 1;
    ReconnectToken generatedToken;
    expect(generateReconnectToken(generatedToken) && isValid(generatedToken)
            && !reconnectTokensEqual(generatedToken, token),
        "reconnect credentials come from the platform cryptographic random source");

    ProtocolEnvelope reconnectEnvelope;
    reconnectEnvelope.sequence = 1;
    ReconnectHello reconnectHello { sessionId, kGuestPlayerId, token, EventSequence { 17 } };
    expect(encodeHandshakeMessage(reconnectHello, reconnectEnvelope) == HandshakeError::None,
        "reconnect handshake encodes the session, slot, credential, and resume boundary");
    HandshakeDecodeResult decodedReconnect = decodeHandshakeMessage(reconnectEnvelope);
    const ReconnectHello* decodedHello = decodedReconnect
        ? std::get_if<ReconnectHello>(&decodedReconnect.message)
        : nullptr;
    expect(decodedHello != nullptr
            && decodedHello->sessionId == sessionId
            && decodedHello->playerId == kGuestPlayerId
            && reconnectTokensEqual(decodedHello->reconnectToken, token)
            && decodedHello->lastAppliedEvent == EventSequence { 17 },
        "reconnect handshake round-trips every authentication and recovery field");
    ReconnectTokenRegistry tokens(sessionId);
    expect(tokens.install(kGuestPlayerId, token) == ReconnectTokenError::None,
        "reconnect registry installs a nonzero token for the guest slot");
    expect(tokens.validate(sessionId, kGuestPlayerId, token),
        "reconnect registry validates the matching session, slot, and token");
    expect(!tokens.validate(SessionId { sessionId.value + 1 }, kGuestPlayerId, token)
            && !tokens.validate(sessionId, kHostPlayerId, token)
            && !tokens.validate(sessionId, kGuestPlayerId, wrongToken),
        "reconnect token cannot be reused for another session, slot, or value");
    tokens.invalidate(kGuestPlayerId);
    expect(!tokens.validate(sessionId, kGuestPlayerId, token), "invalidating a reconnect slot revokes its token");
    ReconnectToken emptyToken;
    expect(tokens.install(kGuestPlayerId, emptyToken) == ReconnectTokenError::InvalidToken,
        "reconnect registry rejects an all-zero token");
    tokens.reset(SessionId { sessionId.value + 1 });
    expect(tokens.sessionId() == SessionId { sessionId.value + 1 }
            && !tokens.validate(sessionId, kGuestPlayerId, token),
        "starting a new session invalidates all old reconnect tokens");

    WorldSnapshot snapshot = sampleSnapshot();
    SnapshotRecoveryQueue queue;
    expect(queue.begin(snapshot) == SnapshotQueueError::None, "snapshot recovery begins from a valid host snapshot");
    expect(queue.append(sampleMovementEvent(42)) == SnapshotQueueError::None
            && queue.append(sampleMovementEvent(43)) == SnapshotQueueError::None,
        "events created during snapshot transfer queue contiguously");
    std::optional<SnapshotRecoveryBatch> batch = queue.finish();
    expect(batch.has_value()
            && batch->snapshot.lastIncludedEvent == EventSequence { 41 }
            && batch->followingEvents.size() == 2
            && batch->followingEvents.back().sequence == EventSequence { 43 },
        "snapshot recovery returns the base snapshot followed by ordered newer events");
    expect(!queue.active(), "finishing recovery clears the in-flight queue");

    expect(queue.begin(snapshot) == SnapshotQueueError::None, "snapshot recovery can restart");
    expect(queue.append(sampleMovementEvent(43)) == SnapshotQueueError::SequenceGap,
        "snapshot recovery rejects a gap after the snapshot boundary");
    SnapshotRecoveryQueue tinyQueue(1);
    expect(tinyQueue.begin(snapshot) == SnapshotQueueError::None, "bounded snapshot queue starts");
    expect(tinyQueue.append(sampleMovementEvent(42)) == SnapshotQueueError::QueueOverflow
            && tinyQueue.restartRequired()
            && !tinyQueue.finish().has_value(),
        "snapshot queue overflow requires a fresh snapshot instead of dropping newer events");
}

void testDialogueVotingPolicies()
{
    DialogueVoteController votes;
    std::vector<PlayerId> threePlayers { PlayerId { 3 }, kGuestPlayerId, kHostPlayerId };
    expect(votes.begin(7, kGuestPlayerId, kHostPlayerId, threePlayers, 3,
               DialogueVotingPolicy::MajorityHostTie, 1000),
        "three-player dialogue starts with a bounded deterministic roster");
    expect(votes.ballots().size() == 3
            && votes.ballots()[0].playerId == kHostPlayerId
            && votes.ballots()[2].playerId == PlayerId { 3 },
        "dialogue ballot ordering is independent of registration order");
    expect(!votes.vote(PlayerId { 4 }, 7, 1)
            && !votes.vote(kGuestPlayerId, 6, 1)
            && !votes.vote(kGuestPlayerId, 7, 3),
        "unknown players, stale rounds, and invalid options cannot vote");
    expect(votes.vote(kGuestPlayerId, 7, 1)
            && votes.vote(PlayerId { 3 }, 7, 1)
            && votes.resolve(10) == std::optional<std::uint8_t> { 1 },
        "a three-player majority resolves without waiting for the minority");
    expect(!votes.vote(kHostPlayerId, 7, 2),
        "a resolved choice cannot be changed or executed twice");

    expect(votes.begin(8, kGuestPlayerId, kHostPlayerId,
               { kHostPlayerId, kGuestPlayerId }, 2,
               DialogueVotingPolicy::MajorityHostTie, 1000)
            && votes.vote(kGuestPlayerId, 8, 1)
            && votes.vote(kHostPlayerId, 8, 0)
            && votes.resolve(10) == std::optional<std::uint8_t> { 0 },
        "the host breaks a two-player majority tie");
    expect(votes.begin(9, kGuestPlayerId, kHostPlayerId,
               { kHostPlayerId, kGuestPlayerId }, 2,
               DialogueVotingPolicy::TalkerDecides, 1000)
            && votes.vote(kGuestPlayerId, 9, 1)
            && !votes.resolve(10).has_value()
            && votes.vote(kHostPlayerId, 9, 0)
            && votes.resolve(10) == std::optional<std::uint8_t> { 1 },
        "the talker decides only after seeing the other player's vote");
    expect(votes.begin(10, kGuestPlayerId, kHostPlayerId,
               { kHostPlayerId, kGuestPlayerId }, 2,
               DialogueVotingPolicy::TalkerDecides, 1000)
            && votes.vote(kGuestPlayerId, 10, 1)
            && votes.setConnected(kHostPlayerId, false)
            && votes.resolve(10) == std::optional<std::uint8_t> { 1 },
        "a disconnected voter abstains without shrinking the frozen roster");
    expect(votes.begin(11, kGuestPlayerId, kHostPlayerId,
               { kHostPlayerId, kGuestPlayerId }, 2,
               DialogueVotingPolicy::HostDecides, 1000)
            && votes.vote(kGuestPlayerId, 11, 1)
            && !votes.resolve(10).has_value()
            && votes.vote(kHostPlayerId, 11, 0)
            && votes.resolve(10) == std::optional<std::uint8_t> { 0 },
        "host-authority policy ignores the guest's preferred option");
    expect(votes.begin(12, kGuestPlayerId, kHostPlayerId,
               { kHostPlayerId, kGuestPlayerId }, 2,
               DialogueVotingPolicy::TalkerDecides, 1000)
            && votes.vote(kHostPlayerId, 12, 1)
            && votes.resolve(1000) == std::optional<std::uint8_t> { 1 },
        "timeout treats a missing talker ballot as an abstention and uses the host vote");

    expect(votes.begin(13, kGuestPlayerId, kHostPlayerId,
               { kHostPlayerId, kGuestPlayerId }, 2,
               DialogueVotingPolicy::MajorityStatsRandomTie, 1000)
            && votes.setTieBreakStats(kHostPlayerId, 5, 9)
            && votes.setTieBreakStats(kGuestPlayerId, 7, 2)
            && votes.vote(kHostPlayerId, 13, 0)
            && votes.vote(kGuestPlayerId, 13, 1)
            && votes.resolve(10) == std::optional<std::uint8_t> { 1 },
        "charisma wins a split dialogue vote before intelligence");
    expect(votes.begin(14, kGuestPlayerId, kHostPlayerId,
               { kHostPlayerId, kGuestPlayerId }, 2,
               DialogueVotingPolicy::MajorityStatsRandomTie, 1000)
            && votes.setTieBreakStats(kHostPlayerId, 7, 5)
            && votes.setTieBreakStats(kGuestPlayerId, 7, 8)
            && votes.vote(kHostPlayerId, 14, 0)
            && votes.vote(kGuestPlayerId, 14, 1)
            && votes.resolve(10) == std::optional<std::uint8_t> { 1 },
        "intelligence wins when tied voters have equal charisma");
    expect(votes.begin(15, kGuestPlayerId, kHostPlayerId,
               { kHostPlayerId, kGuestPlayerId }, 2,
               DialogueVotingPolicy::MajorityStatsRandomTie, 1000)
            && votes.setTieBreakStats(kHostPlayerId, 7, 8)
            && votes.setTieBreakStats(kGuestPlayerId, 7, 8)
            && votes.vote(kHostPlayerId, 15, 0)
            && votes.vote(kGuestPlayerId, 15, 1)
            && !votes.resolve(10).has_value()
            && votes.needsRandomTie()
            && votes.randomTieOptionCount() == 2
            && votes.resolve(10, 1) == std::optional<std::uint8_t> { 1 }
            && !votes.needsRandomTie(),
        "an exact stat tie needs one caller-provided host random draw");
    expect(votes.begin(16, kGuestPlayerId, kHostPlayerId,
               { kHostPlayerId, kGuestPlayerId, PlayerId { 3 } }, 2,
               DialogueVotingPolicy::MajorityStatsRandomTie, 1000)
            && votes.vote(kHostPlayerId, 16, 0)
            && votes.vote(kGuestPlayerId, 16, 1)
            && votes.vote(PlayerId { 3 }, 16, 1)
            && votes.resolve(10) == std::optional<std::uint8_t> { 1 },
        "three-player majority outranks all individual tie-break stats");
}

void testDialogueAndActivityWireRecovery()
{
    GameCommand vote;
    vote.sequence = CommandSequence { 3 };
    vote.playerId = kGuestPlayerId;
    vote.actorId = EntityId { 2 };
    vote.expectedPhase = SessionPhase::Dialogue;
    vote.expectedPhaseRevision = 8;
    vote.payload = DialogueVoteCommand { 42, 1 };
    ProtocolEnvelope envelope = gameplayEnvelope(201);
    expect(encodeGameCommand(vote, envelope) == GameplayWireError::None,
        "dialogue vote command encodes");
    auto decodedVote = decodeGameCommand(envelope);
    expect(decodedVote && std::get<DialogueVoteCommand>(decodedVote.command.payload).revision == 42
            && std::get<DialogueVoteCommand>(decodedVote.command.payload).option == 1,
        "revision-keyed dialogue vote round trips");
    std::get<DialogueVoteCommand>(vote.payload).option = kMaximumDialogueOptions;
    expect(encodeGameCommand(vote, envelope) == GameplayWireError::InvalidModal,
        "dialogue vote rejects an invalid option");

    GameEvent presentationEvent { EventSequence { 51 }, CommandSequence { 3 },
        DialoguePresentationEvent { EntityId { 2 }, EntityId { 12 }, 42, 1,
            "Tell me about the vault.", { "Ask about the chip", "Leave" } } };
    expect(encodeGameEvent(presentationEvent, envelope) == GameplayWireError::None,
        "dialogue presentation event encodes");
    auto decodedPresentation = decodeGameEvent(envelope);
    const auto* presentation = decodedPresentation
        ? std::get_if<DialoguePresentationEvent>(&decodedPresentation.event.payload)
        : nullptr;
    expect(presentation != nullptr && presentation->reply == "Tell me about the vault."
            && presentation->options.size() == 2
            && presentation->options[1] == "Leave",
        "reply and options survive the authoritative event wire");
    envelope.payload[36] = 5;
    expect(decodeGameEvent(envelope).error == GameplayWireError::InvalidModal,
        "unknown dialogue voting policies are rejected on decode");
    envelope.payload[36] = 1;
    envelope.payload.pop_back();
    expect(decodeGameEvent(envelope).error == GameplayWireError::InvalidLength,
        "truncated dialogue text is rejected before indexing it");

    SharedActivityEntry activity { 9, kGuestPlayerId, "Guest",
        SharedActivityKind::Quest, 733, 2, "Quest progressed" };
    GameEvent activityEvent { EventSequence { 52 }, CommandSequence { 3 },
        SharedActivityPublishedEvent { EntityId { 1 }, activity } };
    expect(encodeGameEvent(activityEvent, envelope) == GameplayWireError::None,
        "attributed Pip-Boy activity event encodes");
    auto decodedActivity = decodeGameEvent(envelope);
    const auto* activityPayload = decodedActivity
        ? std::get_if<SharedActivityPublishedEvent>(&decodedActivity.event.payload)
        : nullptr;
    expect(activityPayload != nullptr && activityPayload->entry.id == 9
            && activityPayload->entry.sourceId == kGuestPlayerId
            && activityPayload->entry.sourceName == "Guest"
            && activityPayload->entry.subject == 733,
        "shared activity keeps identity, source, and quest key over the wire");

    WorldSnapshot snapshot = sampleSnapshot();
    snapshot.phase = SessionPhase::Dialogue;
    snapshot.dialogueActorId = EntityId { 2 };
    snapshot.dialoguePresentation = std::get<DialoguePresentationEvent>(presentationEvent.payload);
    snapshot.dialogueBallots = {
        DialogueBallot { kHostPlayerId, std::uint8_t { 0 }, true },
        DialogueBallot { kGuestPlayerId, std::uint8_t { 1 }, true },
    };
    snapshot.sharedActivity.push_back(activity);
    std::vector<std::uint8_t> packet;
    expect(encodeSnapshot(snapshot, packet) == SnapshotError::None,
        "in-flight dialogue and feed encode in a reconnect snapshot");
    auto decoded = decodeSnapshot(packet);
    expect(decoded && decoded.snapshot.dialoguePresentation.has_value()
            && decoded.snapshot.dialoguePresentation->revision == 42
            && decoded.snapshot.dialogueBallots.size() == 2
            && decoded.snapshot.dialogueBallots[1].option == std::uint8_t { 1 }
            && decoded.snapshot.sharedActivity.size() == 1
            && decoded.snapshot.sharedActivity[0].sourceName == "Guest",
        "reconnect restores options, visible votes, and ordered activity");
    snapshot.sharedActivity.push_back(SharedActivityEntry {
        10, PlayerId { 3 }, "Third", SharedActivityKind::Discovery,
        4, 1, "New location discovered" });
    snapshot.sharedActivity.push_back(SharedActivityEntry {
        11, PlayerId {}, "World", SharedActivityKind::WorldOutcome,
        7, 1, "World-map travel ended" });
    expect(encodeSnapshot(snapshot, packet) == SnapshotError::None,
        "three-player and system activity remains valid in one ordered replay");
    decoded = decodeSnapshot(packet);
    expect(decoded && decoded.snapshot.sharedActivity.size() == 3
            && decoded.snapshot.sharedActivity[0].id == 9
            && decoded.snapshot.sharedActivity[0].sourceId == kGuestPlayerId
            && decoded.snapshot.sharedActivity[1].id == 10
            && decoded.snapshot.sharedActivity[1].sourceId == PlayerId { 3 }
            && decoded.snapshot.sharedActivity[2].id == 11
            && decoded.snapshot.sharedActivity[2].sourceId == PlayerId {}
            && decoded.snapshot.sharedActivity[2].sourceName == "World",
        "reconnect preserves feed order, third-player identity, and neutral source");
    WorldSnapshot duplicate = snapshot;
    duplicate.sharedActivity.push_back(snapshot.sharedActivity.back());
    expect(validateSnapshot(duplicate) == SnapshotError::InvalidDialogueState,
        "snapshot rejects duplicate activity identities");
}

} // namespace
} // namespace multiplayer
} // namespace fallout

int main()
{
    fallout::multiplayer::testCoreTypes();
    fallout::multiplayer::testCombatTurnController();
    fallout::multiplayer::testDialogueVotingPolicies();
    fallout::multiplayer::testDialogueAndActivityWireRecovery();
    fallout::multiplayer::testEntityRegistry();
    fallout::multiplayer::testEntityRegistryAcrossEngineLifecycles();
    fallout::multiplayer::testPlayerCharacterStateStore();
    fallout::multiplayer::testCharacterLobbyValidationAndWireFormat();
    fallout::multiplayer::testMultiplayerSaveSidecar();
    fallout::multiplayer::testDirectTradeController();
    fallout::multiplayer::testLootPolicy();
    fallout::multiplayer::testDirectTradeWire();
    fallout::multiplayer::testActingPlayerContext();
    fallout::multiplayer::testLocalPlayerContext();
    fallout::multiplayer::testProtocolRoundTrip();
    fallout::multiplayer::testProtocolRejectsInvalidPackets();
    fallout::multiplayer::testContentManifest();
    fallout::multiplayer::testGameplayWireFormat();
    fallout::multiplayer::testLoopbackTransport();
    fallout::multiplayer::testTcpTransportAndHandshake();
    fallout::multiplayer::testNetworkLaunchAndBootstrap();
    fallout::multiplayer::testNetworkCharacterLobby();
    fallout::multiplayer::testNetworkCombatTurnTransport();
    fallout::multiplayer::testLocalSessionLifecycle();
    fallout::multiplayer::testCombatTurnCommandsAndReplication();
    fallout::multiplayer::testCombatActionCommandsAndReplication();
    fallout::multiplayer::testAuthoritativeCommandProcessing();
    fallout::multiplayer::testSnapshotRoundTripAndRecovery();
    fallout::multiplayer::testNetworkSessionRecoveryPrimitives();

    if (fallout::multiplayer::failures != 0) {
        std::cerr << fallout::multiplayer::failures << " test assertion(s) failed\n";
        return 1;
    }

    std::cout << "multiplayer core tests passed\n";
    return 0;
}
