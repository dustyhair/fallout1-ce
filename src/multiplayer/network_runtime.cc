#include "multiplayer/network_runtime.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agent_journal.h"
#include "game/actions.h"
#include "game/anim.h"
#include "game/combat.h"
#include "game/critter.h"
#include "game/display.h"
#include "game/game.h"
#include "game/gconfig.h"
#include "game/inventry.h"
#include "game/item.h"
#include "game/mainmenu.h"
#include "game/map.h"
#include "game/object.h"
#include "game/protinst.h"
#include "game/proto_types.h"
#include "game/queue.h"
#include "game/textobj.h"
#include "game/tile.h"
#include "multiplayer/character_build_bridge.h"
#include "multiplayer/content_manifest.h"
#include "multiplayer/gameplay_wire.h"
#include "multiplayer/local_player_context.h"
#include "multiplayer/network_bootstrap.h"
#include "multiplayer/network_lobby.h"
#include "multiplayer/network_world.h"
#include "multiplayer/protocol.h"
#include "plib/color/color.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/input.h"
#include "plib/gnw/intrface.h"
#include "plib/gnw/kb.h"
#include "plib/gnw/memory.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/text.h"

namespace fallout {
namespace multiplayer {
namespace {

NetworkLaunchOptions launchOptions;
NetworkBootstrap bootstrap;
NetworkLobby lobby;
ReconnectTokenRegistry reconnectTokens;
NetworkBootstrapState reportedState = NetworkBootstrapState::Disabled;
std::optional<CharacterCreationSheet> pendingLocalSheet;
std::string runtimeStatus;
bool backgroundProcessRegistered = false;
bool lobbyStarted = false;
bool smokeTestEnabled = false;
enum class SmokeScenario {
    Movement,
    Door,
    Pickup,
    Loot,
    PlayerTransfer,
    Skill,
    Scenery,
    Container,
    Quest,
    Elevation,
};
SmokeScenario smokeScenario = SmokeScenario::Movement;

const char* smokeScenarioName()
{
    switch (smokeScenario) {
    case SmokeScenario::Movement:
        return "move";
    case SmokeScenario::Door:
        return "door";
    case SmokeScenario::Pickup:
        return "pickup";
    case SmokeScenario::Loot:
        return "loot";
    case SmokeScenario::PlayerTransfer:
        return "transfer";
    case SmokeScenario::Skill:
        return "skill";
    case SmokeScenario::Scenery:
        return "scenery";
    case SmokeScenario::Container:
        return "container";
    case SmokeScenario::Quest:
        return "quest";
    case SmokeScenario::Elevation:
        return "elevation";
    }
    return "unknown";
}
int lastSentLocalRotation = -1;
std::optional<EventSequence> pendingRecoveryRequest;
std::unique_ptr<Transport> reconnectTransport;
std::chrono::steady_clock::time_point reconnectDeadline;
std::chrono::steady_clock::time_point nextReconnectAttempt;
EventSequence reconnectLastApplied;
std::uint64_t nextHostCommandSequence = 1;
std::chrono::steady_clock::time_point nextAuthoritativeState;
std::chrono::steady_clock::time_point nextAgentWorldReport;

bool queueEventStatesEqual(const std::vector<QueueEventState>& lhs, const std::vector<QueueEventState>& rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); index++) {
        if (lhs[index].time != rhs[index].time
            || lhs[index].eventType != rhs[index].eventType
            || lhs[index].owner != rhs[index].owner
            || lhs[index].payloadCount != rhs[index].payloadCount
            || lhs[index].payload != rhs[index].payload) {
            return false;
        }
    }
    return true;
}

bool smokeTestTimedQueueRoundTrip()
{
    std::vector<QueueEventState> original;
    if (!queue_capture_state(original)) {
        return false;
    }
    ScriptEvent* scriptEvent = (ScriptEvent*)mem_malloc(sizeof(*scriptEvent));
    if (scriptEvent == nullptr) {
        return false;
    }
    scriptEvent->sid = 0x01000042;
    scriptEvent->fixedParam = -7;
    if (queue_add(1000000, nullptr, scriptEvent, EVENT_TYPE_SCRIPT) == -1) {
        mem_free(scriptEvent);
        return false;
    }

    std::vector<QueueEventState> expected;
    std::vector<QueueEventState> actual;
    bool passed = queue_capture_state(expected)
        && queue_replace_state(expected)
        && queue_capture_state(actual)
        && queueEventStatesEqual(expected, actual);
    bool restored = queue_replace_state(original);
    return passed && restored;
}

struct PendingLocalItemDrop {
    Object* source = nullptr;
    Object* item = nullptr;
    EntityId sourceId;
    EntityId currentItemId;
    std::uint32_t remainingQuantity = 0;
};

std::optional<PendingLocalItemDrop> pendingLocalItemDrop;

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void continuePendingLocalItemDrop(const ItemDroppedEvent& drop)
{
    if (launchOptions.mode != NetworkLaunchMode::Join
        || !pendingLocalItemDrop.has_value()
        || drop.actorId != EntityId { kGuestPlayerId.value }
        || drop.sourceId != pendingLocalItemDrop->sourceId
        || (isValid(pendingLocalItemDrop->currentItemId)
            && drop.itemId != pendingLocalItemDrop->currentItemId)
        || drop.quantity > pendingLocalItemDrop->remainingQuantity) {
        return;
    }

    pendingLocalItemDrop->remainingQuantity -= drop.quantity;
    if (pendingLocalItemDrop->remainingQuantity == 0) {
        pendingLocalItemDrop.reset();
        return;
    }

    std::uint32_t nextSourceQuantity = drop.sourceQuantity - drop.quantity;
    if (!isValid(drop.remainderItemId)
        || nextSourceQuantity == 0
        || !lobby.sendLocalItemDrop(drop.sourceId,
            drop.remainderItemId,
            1,
            nextSourceQuantity,
            networkWorldPhaseRevision())) {
        debug_printf("Multiplayer queued item drop could not continue.\n");
        pendingLocalItemDrop.reset();
        return;
    }
    pendingLocalItemDrop->currentItemId = drop.remainderItemId;
}

void hashBytes(std::uint64_t& digest, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; index++) {
        digest ^= bytes[index];
        digest *= kFnvPrime;
    }
}

void hashString(std::uint64_t& digest, const char* value)
{
    if (value != nullptr) {
        hashBytes(digest, value, std::char_traits<char>::length(value));
    }
    const std::uint8_t separator = 0;
    hashBytes(digest, &separator, sizeof(separator));
}

void hashUInt64(std::uint64_t& digest, std::uint64_t value)
{
    std::array<std::uint8_t, 8> bytes;
    for (int index = 7; index >= 0; index--) {
        bytes[index] = static_cast<std::uint8_t>(value & 0xFF);
        value >>= 8;
    }
    hashBytes(digest, bytes.data(), bytes.size());
}

std::uint64_t compatibilityDigest()
{
    char* masterDat = nullptr;
    char* critterDat = nullptr;
    char* masterPatches = nullptr;
    char* critterPatches = nullptr;
    char* language = nullptr;
    int gameDifficulty = 0;
    int combatDifficulty = 0;
    int violenceLevel = 0;
    int interruptWalk = 0;
    int objectHashing = 0;
    if (!config_get_string(&game_config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_MASTER_DAT_KEY, &masterDat)
        || !config_get_string(&game_config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_CRITTER_DAT_KEY, &critterDat)
        || !config_get_string(&game_config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_MASTER_PATCHES_KEY, &masterPatches)
        || !config_get_string(&game_config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_CRITTER_PATCHES_KEY, &critterPatches)
        || !config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_GAME_DIFFICULTY_KEY, &gameDifficulty)
        || !config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_DIFFICULTY_KEY, &combatDifficulty)
        || !config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_VIOLENCE_LEVEL_KEY, &violenceLevel)
        || !config_get_value(&game_config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_INTERRUPT_WALK_KEY, &interruptWalk)
        || !config_get_value(&game_config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_HASHING_KEY, &objectHashing)) {
        return 0;
    }
    config_get_string(&game_config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_LANGUAGE_KEY, &language);

    ContentManifestResult manifest = buildContentManifest(
        masterDat,
        critterDat,
        masterPatches,
        critterPatches);
    if (!manifest) {
        debug_printf("Multiplayer content manifest failed: %s.\n",
            contentManifestErrorMessage(manifest.error));
        return 0;
    }

    std::uint64_t digest = kFnvOffsetBasis;
    hashString(digest, "fallout-ce-multiplayer-compatibility-v2");
    hashUInt64(digest, kProtocolVersion);
    hashUInt64(digest, kConnectionHandshakeVersion);
    hashUInt64(digest, kGameplayWireVersion);
    hashUInt64(digest, kNetworkLobbyVersion);
    hashUInt64(digest, kSnapshotVersion);
    hashString(digest, language);
    hashUInt64(digest, static_cast<std::uint64_t>(gameDifficulty));
    hashUInt64(digest, static_cast<std::uint64_t>(combatDifficulty));
    hashUInt64(digest, static_cast<std::uint64_t>(violenceLevel));
    hashUInt64(digest, static_cast<std::uint64_t>(interruptWalk));
    hashUInt64(digest, static_cast<std::uint64_t>(objectHashing));
    hashUInt64(digest, manifest.digest);
    return digest != 0 ? digest : 1;
}

SessionId createSessionId()
{
    std::uint64_t value = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    value ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&bootstrap));
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    return SessionId { value != 0 ? value : 1 };
}

void setStatus(const std::string& status)
{
    if (runtimeStatus == status) {
        return;
    }
    runtimeStatus = status;
    agentJournalWriteText("multiplayer_status", runtimeStatus.c_str());
    main_menu_set_multiplayer_status(runtimeStatus.c_str());
    std::fprintf(stderr, "%s\n", runtimeStatus.c_str());
    debug_printf("%s\n", runtimeStatus.c_str());
}

std::string playerLabel(PlayerId playerId)
{
    return playerId == kHostPlayerId ? "HOST" : "GUEST";
}

const CharacterCreationSheet* playerSheet(PlayerId playerId)
{
    const CharacterCreationSheet* local = lobby.localSheet();
    if (local != nullptr && local->playerId == playerId) {
        return local;
    }
    const CharacterCreationSheet* peer = lobby.peerSheet();
    if (peer != nullptr && peer->playerId == playerId) {
        return peer;
    }
    return nullptr;
}

std::string trimmedChatText(const char* input)
{
    std::string text = input != nullptr ? input : "";
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    text.erase(std::find_if(text.rbegin(), text.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), text.end());
    return text;
}

void presentGameChatMessage(const LobbyChatMessage& message, const char* direction)
{
    const CharacterCreationSheet* sheet = playerSheet(message.playerId);
    std::string name = sheet != nullptr ? sheet->name : playerLabel(message.playerId);
    agentJournalWriteChat(direction, message.playerId.value, name.c_str(), message.text.c_str());
    std::array<char, 128> monitorText = {};
    std::snprintf(monitorText.data(), monitorText.size(), "%s: %s", name.c_str(), message.text.c_str());
    display_print(monitorText.data());

    Object* actor = networkWorldPlayerActor(message.playerId);
    if (actor == nullptr || actor->elevation != map_elevation) {
        return;
    }
    std::array<char, kMaxLobbyChatMessageLength + 1> floatingText = {};
    std::snprintf(floatingText.data(), floatingText.size(), "%s", message.text.c_str());
    Rect rect;
    if (text_object_create(actor,
            floatingText.data(),
            101,
            colorTable[32747],
            colorTable[0],
            &rect)
        != -1) {
        tile_refresh_rect(&rect, actor->elevation);
    }
}

void presentPendingGameChatMessages()
{
    while (std::optional<LobbyChatMessage> message = lobby.takeChatMessage()) {
        presentGameChatMessage(*message, "incoming");
    }
}

const char* phaseLabel(SessionPhase phase)
{
    switch (phase) {
    case SessionPhase::Lobby:
        return "lobby";
    case SessionPhase::Loading:
        return "loading";
    case SessionPhase::Exploration:
        return "exploration";
    case SessionPhase::Combat:
        return "combat";
    case SessionPhase::Dialogue:
        return "dialogue";
    case SessionPhase::Transition:
        return "transition";
    case SessionPhase::Ending:
        return "ending";
    }
    return "unknown";
}

AgentJournalActorState actorJournalState(PlayerId playerId, bool local)
{
    AgentJournalActorState state;
    state.playerId = playerId.value;
    state.local = local;
    const CharacterCreationSheet* sheet = playerSheet(playerId);
    state.name = sheet != nullptr ? sheet->name : playerLabel(playerId);
    Object* actor = networkWorldPlayerActor(playerId);
    if (actor != nullptr) {
        if (std::optional<EntityId> entityId = networkWorldFindEntity(actor)) {
            state.entityId = entityId->value;
        }
        state.tile = actor->tile;
        state.elevation = actor->elevation;
        state.rotation = actor->rotation;
        if (actor->elevation == map_elevation) {
            Rect bounds;
            obj_bound(actor, &bounds);
            state.screenX = (bounds.ulx + bounds.lrx) / 2;
            state.screenY = (bounds.uly + bounds.lry) / 2;
        }
        state.hitPoints = critter_get_hits(actor);
        state.actionPoints = actor->data.critter.combat.ap;
    }
    return state;
}

bool isOnScreen(const Rect& bounds)
{
    return bounds.lrx >= 0
        && bounds.lry >= 0
        && bounds.ulx < screenGetWidth()
        && bounds.uly < screenGetHeight();
}

std::string critterDisposition(Object* critter, Object* localActor, Object* hostActor, Object* guestActor)
{
    if (critter_is_dead(critter)) {
        return "dead";
    }
    int playerTeam = localActor->data.critter.combat.team;
    if (critter->data.critter.combat.team == playerTeam) {
        return "friendly";
    }

    Object* whoHitCritter = critter->data.critter.combat.whoHitMe;
    bool attackedByPlayer = whoHitCritter == hostActor || whoHitCritter == guestActor;
    auto wasAttackedByCritterTeam = [&](Object* player) {
        Object* attacker = player != nullptr ? player->data.critter.combat.whoHitMe : nullptr;
        return attacker != nullptr
            && attacker->data.critter.combat.team == critter->data.critter.combat.team;
    };
    if (attackedByPlayer
        || wasAttackedByCritterTeam(hostActor)
        || wasAttackedByCritterTeam(guestActor)
        || combat_is_critter_involved(critter)) {
        return "hostile";
    }
    return "neutral";
}

std::vector<AgentJournalCritterState> visibleCritterJournalStates(Object* localActor,
    Object* hostActor,
    Object* guestActor)
{
    std::vector<AgentJournalCritterState> states;
    Object** critters = nullptr;
    int critterCount = obj_create_list(-1, map_elevation, OBJ_TYPE_CRITTER, &critters);
    for (int index = 0; index < critterCount; index++) {
        Object* critter = critters[index];
        if (critter == nullptr
            || critter == hostActor
            || critter == guestActor
            || critter->tile < 0
            || (critter->flags & OBJECT_HIDDEN) != 0) {
            continue;
        }
        Rect bounds;
        obj_bound(critter, &bounds);
        if (!isOnScreen(bounds)) {
            continue;
        }

        AgentJournalCritterState state;
        if (std::optional<EntityId> entityId = networkWorldFindEntity(critter)) {
            state.entityId = entityId->value;
        }
        state.pid = critter->pid;
        const char* name = object_name(critter);
        state.name = name != nullptr ? name : "";
        state.disposition = critterDisposition(critter, localActor, hostActor, guestActor);
        state.team = critter->data.critter.combat.team;
        state.tile = critter->tile;
        state.elevation = critter->elevation;
        state.rotation = critter->rotation;
        state.screenX = (bounds.ulx + bounds.lrx) / 2;
        state.screenY = (bounds.uly + bounds.lry) / 2;
        state.distance = obj_dist(localActor, critter);
        state.hitPoints = critter_get_hits(critter);
        states.push_back(std::move(state));
    }
    if (critters != nullptr) {
        obj_delete_list(critters);
    }
    std::sort(states.begin(), states.end(), [](const auto& left, const auto& right) {
        if (left.entityId != right.entityId) {
            return left.entityId < right.entityId;
        }
        if (left.tile != right.tile) {
            return left.tile < right.tile;
        }
        return left.pid < right.pid;
    });
    return states;
}

std::vector<AgentJournalInventoryItemState> localInventoryJournalStates(Object* localActor)
{
    std::vector<AgentJournalInventoryItemState> states;
    if (localActor == nullptr) {
        return states;
    }
    const Inventory& inventory = localActor->data.inventory;
    for (int index = 0; index < inventory.length; index++) {
        Object* item = inventory.items[index].item;
        if (item == nullptr || inventory.items[index].quantity <= 0) {
            continue;
        }
        std::optional<EntityId> entityId = networkWorldFindEntity(item);
        if (!entityId.has_value()) {
            continue;
        }
        AgentJournalInventoryItemState state;
        state.entityId = entityId->value;
        state.pid = item->pid;
        const char* name = object_name(item);
        state.name = name != nullptr ? name : "";
        state.quantity = static_cast<std::uint32_t>(inventory.items[index].quantity);
        state.equipped = (item->flags & (OBJECT_EQUIPPED | OBJECT_USED)) != 0;
        states.push_back(std::move(state));
    }
    std::sort(states.begin(), states.end(), [](const auto& left, const auto& right) {
        return left.entityId < right.entityId;
    });
    return states;
}

std::vector<AgentJournalInteractableState> visibleInteractableJournalStates(Object* localActor)
{
    std::vector<AgentJournalInteractableState> states;
    auto appendType = [&](int objectType) {
        Object** objects = nullptr;
        int objectCount = obj_create_list(-1, map_elevation, objectType, &objects);
        for (int index = 0; index < objectCount; index++) {
            Object* object = objects[index];
            if (object == nullptr
                || object->owner != nullptr
                || object->tile < 0
                || (object->flags & OBJECT_HIDDEN) != 0) {
                continue;
            }
            bool door = obj_is_a_portal(object);
            bool container = objectType == OBJ_TYPE_ITEM
                && item_get_type(object) == ITEM_TYPE_CONTAINER;
            std::optional<EntityId> entityId = networkWorldFindEntity(object);
            if (!entityId.has_value()) {
                continue;
            }
            Rect bounds;
            obj_bound(object, &bounds);
            if (!isOnScreen(bounds)) {
                continue;
            }

            AgentJournalInteractableState state;
            state.entityId = entityId->value;
            state.pid = object->pid;
            state.kind = door ? "door" : container ? "container"
                                                   : objectType == OBJ_TYPE_SCENERY ? "scenery"
                                                                                  : "item";
            const char* name = object_name(object);
            state.name = name != nullptr ? name : "";
            state.tile = object->tile;
            state.elevation = object->elevation;
            state.screenX = (bounds.ulx + bounds.lrx) / 2;
            state.screenY = (bounds.uly + bounds.lry) / 2;
            state.distance = obj_dist(localActor, object);
            if (door || container) {
                state.open = obj_is_open(object) != 0;
                state.locked = obj_is_locked(object);
            }
            states.push_back(std::move(state));
        }
        if (objects != nullptr) {
            obj_delete_list(objects);
        }
    };
    appendType(OBJ_TYPE_SCENERY);
    appendType(OBJ_TYPE_ITEM);
    std::sort(states.begin(), states.end(), [](const auto& left, const auto& right) {
        if (left.entityId != right.entityId) {
            return left.entityId < right.entityId;
        }
        if (left.tile != right.tile) {
            return left.tile < right.tile;
        }
        return left.pid < right.pid;
    });
    return states;
}

void reportAgentWorldState()
{
    if (!agentJournalEnabled()) {
        return;
    }
    if (!networkWorldActive()) {
        agentJournalWriteWorldExit();
        nextAgentWorldReport = {};
        return;
    }
    auto now = std::chrono::steady_clock::now();
    if (nextAgentWorldReport.time_since_epoch().count() != 0 && now < nextAgentWorldReport) {
        return;
    }
    nextAgentWorldReport = now + std::chrono::milliseconds(200);
    bool hostIsLocal = launchOptions.mode == NetworkLaunchMode::Host;
    AgentJournalWorldState state;
    state.map = map_data.name;
    state.phase = phaseLabel(networkWorldPhase());
    state.connected = networkRuntimeConnected();
    state.combat = isInCombat();
    state.host = actorJournalState(kHostPlayerId, hostIsLocal);
    state.guest = actorJournalState(kGuestPlayerId, !hostIsLocal);
    Object* hostActor = networkWorldPlayerActor(kHostPlayerId);
    Object* guestActor = networkWorldPlayerActor(kGuestPlayerId);
    Object* localActor = hostIsLocal ? hostActor : guestActor;
    if (localActor != nullptr) {
        state.localInventory = localInventoryJournalStates(localActor);
        state.visibleCritters = visibleCritterJournalStates(localActor, hostActor, guestActor);
        state.visibleInteractables = visibleInteractableJournalStates(localActor);
    }
    agentJournalWriteWorldState(state);
}

void reportLobbyStatus()
{
    NetworkLobbyState state = lobby.state();
    const CharacterCreationSheet* local = lobby.localSheet();
    const CharacterCreationSheet* peer = lobby.peerSheet();

    if (state == NetworkLobbyState::Waiting) {
        if (local != nullptr && peer == nullptr) {
            setStatus("MULTIPLAYER: CHARACTER SENT, WAITING FOR THE OTHER PLAYER");
        } else if (local == nullptr && peer != nullptr) {
            setStatus("MULTIPLAYER: OTHER PLAYER READY, CHOOSE NEW GAME");
        } else {
            setStatus("MULTIPLAYER CONNECTED: BOTH PLAYERS CHOOSE NEW GAME");
        }
    } else if (state == NetworkLobbyState::Ready && local != nullptr && peer != nullptr) {
        const CharacterCreationSheet* host = local->playerId == kHostPlayerId ? local : peer;
        const CharacterCreationSheet* guest = local->playerId == kGuestPlayerId ? local : peer;
        if (lobby.startRequested()) {
            setStatus("MULTIPLAYER GAME STARTED: " + host->name + " + " + guest->name);
        } else {
            setStatus("MULTIPLAYER LOBBY READY: " + host->name + " + " + guest->name);
        }
    } else if (state == NetworkLobbyState::Disconnected) {
        setStatus(launchOptions.mode == NetworkLaunchMode::Host
                ? "MULTIPLAYER: GUEST DISCONNECTED, WAITING FOR RECONNECT"
                : "MULTIPLAYER: CONNECTION LOST, RECONNECTING");
    } else if (state == NetworkLobbyState::Rejected) {
        setStatus(std::string("MULTIPLAYER CHARACTER REJECTED: ") + characterLobbyErrorMessage(lobby.sheetError()));
    } else if (state == NetworkLobbyState::Failed) {
        setStatus(std::string("MULTIPLAYER LOBBY FAILED: ") + networkLobbyErrorMessage(lobby.error()));
    }
}

bool sendReconnectHandshake(Transport& transport, const HandshakeMessage& message)
{
    ProtocolEnvelope envelope;
    envelope.sequence = 1;
    Packet packet;
    return encodeHandshakeMessage(message, envelope) == HandshakeError::None
        && encodeEnvelope(envelope, packet) == ProtocolError::None
        && transport.send(std::move(packet)) == TransportSendResult::Sent;
}

std::optional<HandshakeMessage> receiveReconnectHandshake(Transport& transport)
{
    std::optional<Packet> packet = transport.receive();
    if (!packet.has_value()) {
        return std::nullopt;
    }
    ProtocolDecodeResult envelope = decodeEnvelope(*packet);
    if (!envelope || envelope.envelope.sequence != 1) {
        transport.close();
        return std::nullopt;
    }
    HandshakeDecodeResult handshake = decodeHandshakeMessage(envelope.envelope);
    if (!handshake) {
        transport.close();
        return std::nullopt;
    }
    return std::move(handshake.message);
}

void discardReconnectTransport()
{
    if (reconnectTransport != nullptr) {
        reconnectTransport->close();
        reconnectTransport.reset();
    }
}

void pollReconnect()
{
    if (lobby.state() != NetworkLobbyState::Disconnected) {
        discardReconnectTransport();
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (reconnectTransport != nullptr
        && (now >= reconnectDeadline || !reconnectTransport->isConnected())) {
        discardReconnectTransport();
        nextReconnectAttempt = now + std::chrono::milliseconds(500);
    }

    if (launchOptions.mode == NetworkLaunchMode::Host) {
        if (reconnectTransport == nullptr) {
            reconnectTransport = bootstrap.acceptReconnectTransport();
            if (reconnectTransport == nullptr) {
                return;
            }
            reconnectDeadline = now + std::chrono::seconds(5);
        }

        std::optional<HandshakeMessage> message = receiveReconnectHandshake(*reconnectTransport);
        if (!message.has_value()) {
            return;
        }
        const ReconnectHello* reconnect = std::get_if<ReconnectHello>(&*message);
        if (reconnect == nullptr
            || reconnect->sessionId != bootstrap.sessionId()
            || reconnect->playerId != kGuestPlayerId
            || !reconnectTokens.validate(reconnect->sessionId, reconnect->playerId, reconnect->reconnectToken)
            || reconnect->lastAppliedEvent.value > lobby.latestAuthoritativeEvent().value) {
            sendReconnectHandshake(*reconnectTransport,
                HandshakeRejected { HandshakeRejection::InvalidReconnect });
            discardReconnectTransport();
            return;
        }
        EventSequence lastApplied = reconnect->lastAppliedEvent;
        if (!sendReconnectHandshake(*reconnectTransport,
                ReconnectWelcome { bootstrap.sessionId(), lobby.latestAuthoritativeEvent() })
            || !lobby.reattachTransport(std::move(reconnectTransport))
            || !lobby.queueRecovery(lastApplied)) {
            discardReconnectTransport();
            return;
        }
        setStatus("MULTIPLAYER: GUEST RECONNECTED, RESYNCHRONIZING");
        return;
    }

    if (reconnectTransport == nullptr) {
        if (now < nextReconnectAttempt) {
            return;
        }
        std::optional<TransportPeerIdentity> identity = bootstrap.peerIdentity();
        if (!identity.has_value()) {
            setStatus("MULTIPLAYER RECONNECT FAILED: MISSING HOST IDENTITY");
            return;
        }
        TcpConnectResult connection = connectTcp(
            launchOptions.address, launchOptions.port, 250, identity);
        if (!connection) {
            nextReconnectAttempt = now + std::chrono::milliseconds(500);
            return;
        }
        reconnectTransport = std::move(connection.transport);
        reconnectLastApplied = lobby.lastAppliedEvent();
        reconnectDeadline = now + std::chrono::seconds(5);
        if (!sendReconnectHandshake(*reconnectTransport,
                ReconnectHello { bootstrap.sessionId(), kGuestPlayerId,
                    bootstrap.reconnectToken(), reconnectLastApplied })) {
            discardReconnectTransport();
            nextReconnectAttempt = now + std::chrono::milliseconds(500);
            return;
        }
    }

    std::optional<HandshakeMessage> message = receiveReconnectHandshake(*reconnectTransport);
    if (!message.has_value()) {
        return;
    }
    const ReconnectWelcome* welcome = std::get_if<ReconnectWelcome>(&*message);
    if (welcome == nullptr
        || welcome->sessionId != bootstrap.sessionId()
        || welcome->latestEvent.value < reconnectLastApplied.value
        || !lobby.reattachTransport(std::move(reconnectTransport))
        || !lobby.beginReconnectRecovery(reconnectLastApplied)) {
        discardReconnectTransport();
        nextReconnectAttempt = now + std::chrono::milliseconds(500);
        return;
    }
    setStatus("MULTIPLAYER: RECONNECTED, RESYNCHRONIZING");
}

bool startLobby()
{
    std::unique_ptr<Transport> transport = bootstrap.takeTransport();
    lobbyStarted = true;
    if (!lobby.start(launchOptions.mode, bootstrap.sessionId(), std::move(transport))) {
        setStatus(std::string("MULTIPLAYER LOBBY FAILED: ") + networkLobbyErrorMessage(lobby.error()));
        return false;
    }
    if (pendingLocalSheet.has_value()) {
        CharacterLobbyError error = lobby.submitLocalSheet(*pendingLocalSheet);
        if (error != CharacterLobbyError::None) {
            setStatus(std::string("MULTIPLAYER CHARACTER REJECTED: ") + characterLobbyErrorMessage(error));
            return false;
        }
    }
    reportLobbyStatus();
    return true;
}

void reportState()
{
    NetworkBootstrapState state = bootstrap.state();
    if (state == reportedState) {
        return;
    }
    reportedState = state;

    switch (state) {
    case NetworkBootstrapState::Connected:
        setStatus("MULTIPLAYER CONNECTED: BOTH PLAYERS CHOOSE NEW GAME");
        break;
    case NetworkBootstrapState::Rejected:
        setStatus(std::string("MULTIPLAYER CONNECTION REJECTED: ") + handshakeRejectionMessage(bootstrap.rejection()));
        break;
    case NetworkBootstrapState::Failed:
        setStatus(std::string("MULTIPLAYER CONNECTION FAILED: ") + networkBootstrapErrorMessage(bootstrap.error()));
        break;
    default:
        break;
    }
}

bool submitHostCommand(GameCommandPayload payload)
{
    if (launchOptions.mode != NetworkLaunchMode::Host || !networkWorldActive()) {
        return false;
    }
    GameCommand command;
    command.sequence.value = nextHostCommandSequence++;
    command.playerId = kHostPlayerId;
    command.actorId = EntityId { kHostPlayerId.value };
    command.expectedPhase = std::holds_alternative<AttackCommand>(payload)
        ? SessionPhase::Combat
        : std::holds_alternative<SharedModalCommand>(payload)
        ? networkWorldPhase()
        : SessionPhase::Exploration;
    command.expectedPhaseRevision = networkWorldPhaseRevision();
    command.payload = std::move(payload);

    AuthoritativeCommandResult outcome = networkWorldProcessCommand(command);
    bool accepted = outcome.result.status == CommandStatus::Accepted;
    if (!accepted) {
        debug_printf("Multiplayer host command %llu rejected with reason %d.\n",
            static_cast<unsigned long long>(command.sequence.value),
            static_cast<int>(outcome.result.rejection));
    }
    if (!lobby.publishLocalCommandOutcome(std::move(outcome))) {
        debug_printf("Multiplayer host command outcome could not be published.\n");
        return false;
    }
    return accepted;
}

void networkRuntimeBackgroundProcess()
{
    if (!lobbyStarted) {
        bootstrap.poll();
        reportState();
        if (bootstrap.state() == NetworkBootstrapState::Connected) {
            startLobby();
        }
        reportAgentWorldState();
        return;
    }

    lobby.poll();
    pollReconnect();
    if (lobby.state() == NetworkLobbyState::Disconnected) {
        pendingLocalItemDrop.reset();
    }
    if (networkWorldActive()) {
        presentPendingGameChatMessages();
        if (launchOptions.mode == NetworkLaunchMode::Host) {
            if (!networkWorldSynchronizeEnginePhase()) {
                debug_printf("Multiplayer session phase could not follow the engine phase.\n");
            }
            while (std::optional<GameEvent> event = networkWorldTakeDeferredEvent()) {
                if (!lobby.publishDeferredEvent(std::move(*event))) {
                    debug_printf("Multiplayer deferred authoritative event could not be published.\n");
                    break;
                }
            }
            if (!pendingRecoveryRequest.has_value()) {
                pendingRecoveryRequest = lobby.takeRecoveryRequest();
            }
            if (pendingRecoveryRequest.has_value()) {
                WorldSnapshot snapshot;
                EventReplay replay = lobby.replayAfter(*pendingRecoveryRequest);
                bool snapshotReady = replay.status != EventReplayStatus::SnapshotRequired
                    || networkWorldCaptureSnapshot(lobby.latestAuthoritativeEvent(), snapshot);
                if (snapshotReady) {
                    if (!lobby.sendRecovery(*pendingRecoveryRequest, snapshot)) {
                        debug_printf("Multiplayer recovery response could not be sent.\n");
                    }
                    pendingRecoveryRequest.reset();
                }
            }
            while (std::optional<GameCommand> command = lobby.takePeerCommand()) {
                AuthoritativeCommandResult outcome = networkWorldProcessCommand(*command);
                if (!lobby.sendCommandOutcome(std::move(outcome))) {
                    debug_printf("Multiplayer command outcome could not be sent.\n");
                    break;
                }
            }
            auto now = std::chrono::steady_clock::now();
            if (lobby.state() == NetworkLobbyState::Ready
                && (nextAuthoritativeState.time_since_epoch().count() == 0 || now >= nextAuthoritativeState)) {
                WorldSnapshot state;
                if (networkWorldCaptureAuthoritativeState(lobby.latestAuthoritativeEvent(), state)
                    && !lobby.sendAuthoritativeState(state)) {
                    debug_printf("Multiplayer authoritative state could not be sent.\n");
                }
                nextAuthoritativeState = now + std::chrono::milliseconds(250);
            }
        }
        while (std::optional<WorldSnapshot> snapshot = lobby.takePeerSnapshot()) {
            if (!networkWorldApplySnapshot(*snapshot)) {
                debug_printf("Multiplayer recovery snapshot could not be applied.\n");
                lobby.abortRecovery();
                break;
            }
            pendingLocalItemDrop.reset();
            if (!lobby.confirmSnapshotApplied(snapshot->lastIncludedEvent)) {
                debug_printf("Multiplayer recovery snapshot boundary could not be confirmed.\n");
                lobby.abortRecovery();
                break;
            }
        }
        Object* localActor = localPlayerActor();
        if (localActor != nullptr
            && networkWorldPhase() == SessionPhase::Exploration
            && FID_ANIM_TYPE(localActor->fid) == ANIM_STAND
            && localActor->rotation != lastSentLocalRotation
            && (launchOptions.mode == NetworkLaunchMode::Host
                    ? submitHostCommand(FaceCommand { localActor->rotation })
                    : lobby.sendLocalFacing(localActor->rotation, networkWorldPhaseRevision()))) {
            lastSentLocalRotation = localActor->rotation;
        }
        while (std::optional<CommandResult> result = lobby.takeCommandResult()) {
            if (result->status == CommandStatus::Rejected) {
                debug_printf("Multiplayer command %llu rejected with reason %d.\n",
                    static_cast<unsigned long long>(result->commandSequence.value),
                    static_cast<int>(result->rejection));
                pendingLocalItemDrop.reset();
            }
        }
        while (std::optional<GameEvent> event = lobby.takePeerEvent()) {
            bool applied = false;
            if (const auto* movement = std::get_if<ActorMovementStartedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerMove(*movement);
            } else if (const auto* facing = std::get_if<ActorFacingChangedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerFacing(*facing);
            } else if (const auto* doorUse = std::get_if<DoorUseStartedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerDoorUse(*doorUse);
            } else if (const auto* pickup = std::get_if<ItemPickupStartedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerPickup(*pickup);
            } else if (const auto* pickup = std::get_if<ItemPickupCompletedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerPickupCompletion(*pickup);
            } else if (const auto* loot = std::get_if<LootStartedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerLoot(*loot);
            } else if (const auto* skill = std::get_if<SkillUseStartedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerSkillUse(*skill);
            } else if (const auto* itemUse = std::get_if<ItemUseStartedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerItemUse(*itemUse);
            } else if (const auto* elevator = std::get_if<ElevatorTransitionedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerElevator(*elevator);
            } else if (const auto* modal = std::get_if<SharedModalStateChangedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerSharedModal(*modal);
            } else if (const auto* transfer = std::get_if<InventoryTransferredEvent>(&event->payload)) {
                applied = networkWorldApplyInventoryTransfer(*transfer);
            } else if (const auto* drop = std::get_if<ItemDroppedEvent>(&event->payload)) {
                applied = networkWorldApplyItemDrop(*drop);
            } else if (const auto* attack = std::get_if<AttackStartedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerAttack(*attack);
            }
            if (!applied) {
                debug_printf("Multiplayer peer event could not be applied.\n");
            } else {
                if (const auto* drop = std::get_if<ItemDroppedEvent>(&event->payload)) {
                    continuePendingLocalItemDrop(*drop);
                }
                if (!lobby.confirmPeerEventApplied(event->sequence)) {
                    debug_printf("Multiplayer peer event boundary could not be confirmed.\n");
                }
            }
        }
        while (std::optional<WorldSnapshot> state = lobby.takeAuthoritativeState()) {
            if (!networkWorldApplyAuthoritativeState(*state)) {
                debug_printf("Multiplayer authoritative state could not be applied.\n");
            }
        }
    }
    reportAgentWorldState();
    reportLobbyStatus();
}

bool startConfiguredRuntime()
{
    std::uint64_t digest = compatibilityDigest();
    if (digest == 0) {
        setStatus("MULTIPLAYER CONNECTION FAILED: COULD NOT FINGERPRINT GAME DATA");
        return false;
    }

    SessionId sessionId = launchOptions.mode == NetworkLaunchMode::Host ? createSessionId() : SessionId {};
    if (!bootstrap.start(launchOptions, digest, sessionId)) {
        setStatus(std::string("MULTIPLAYER CONNECTION FAILED: ") + networkBootstrapErrorMessage(bootstrap.error()));
        return false;
    }
    reconnectTokens.reset(sessionId);
    if (launchOptions.mode == NetworkLaunchMode::Host
        && reconnectTokens.install(kGuestPlayerId, bootstrap.reconnectToken()) != ReconnectTokenError::None) {
        setStatus("MULTIPLAYER CONNECTION FAILED: COULD NOT INSTALL RECONNECT CREDENTIAL");
        bootstrap.stop();
        return false;
    }

    reportedState = bootstrap.state();
    if (launchOptions.mode == NetworkLaunchMode::Host) {
        std::string status = "MULTIPLAYER HOST: WAITING ON PORT " + std::to_string(bootstrap.port());
        if (std::optional<TransportPeerIdentity> identity = bootstrap.localIdentity()) {
            status += " FINGERPRINT " + formatTransportPeerIdentity(*identity);
        }
        setStatus(status);
    } else {
        setStatus("MULTIPLAYER GUEST: CONNECTING TO " + launchOptions.address + ":" + std::to_string(launchOptions.port)
            + (launchOptions.expectedHostIdentity.has_value() ? " WITH VERIFIED FINGERPRINT" : " USING TOFU"));
    }

    if (!backgroundProcessRegistered) {
        add_bk_process(networkRuntimeBackgroundProcess);
        backgroundProcessRegistered = true;
    }
    set_background_processing_when_inactive(true);
    return true;
}

} // namespace

bool networkRuntimeConfigure(int argc, char** argv)
{
    NetworkLaunchParseResult result = parseNetworkLaunchOptions(argc, argv);
    if (!result) {
        std::fprintf(stderr, "Invalid multiplayer launch options: %s.\n", networkLaunchParseErrorMessage(result.error));
        return false;
    }
    launchOptions = result.options;
    smokeTestEnabled = false;
    smokeScenario = SmokeScenario::Movement;
    for (int index = 1; index < argc; index++) {
        if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-test") == 0) {
            smokeTestEnabled = true;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=door") == 0) {
            smokeScenario = SmokeScenario::Door;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=pickup") == 0) {
            smokeScenario = SmokeScenario::Pickup;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=loot") == 0) {
            smokeScenario = SmokeScenario::Loot;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=transfer") == 0) {
            smokeScenario = SmokeScenario::PlayerTransfer;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=skill") == 0) {
            smokeScenario = SmokeScenario::Skill;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=scenery") == 0) {
            smokeScenario = SmokeScenario::Scenery;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=container") == 0) {
            smokeScenario = SmokeScenario::Container;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=quest") == 0) {
            smokeScenario = SmokeScenario::Quest;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=elevation") == 0) {
            smokeScenario = SmokeScenario::Elevation;
        }
    }
    if (smokeTestEnabled && launchOptions.mode == NetworkLaunchMode::Disabled) {
        std::fprintf(stderr, "--multiplayer-smoke-test requires --multiplayer-host or --multiplayer-join.\n");
        return false;
    }
    pendingLocalSheet.reset();
    pendingRecoveryRequest.reset();
    pendingLocalItemDrop.reset();
    discardReconnectTransport();
    reconnectLastApplied = {};
    nextHostCommandSequence = 1;
    reconnectTokens.invalidateAll();
    nextReconnectAttempt = {};
    nextAuthoritativeState = {};
    nextAgentWorldReport = {};
    runtimeStatus.clear();
    lobbyStarted = false;
    return true;
}

bool networkRuntimeSmokeTestEnabled()
{
    return smokeTestEnabled;
}

const char* networkRuntimeSmokeTestMap()
{
    if (smokeScenario == SmokeScenario::Quest) {
        return "ShadyW.map";
    }
    return smokeScenario == SmokeScenario::Elevation ? "Vault13.map" : "V13Ent.map";
}

bool networkRuntimeRunSmokeTest()
{
    if (!smokeTestEnabled || launchOptions.mode == NetworkLaunchMode::Disabled) {
        return false;
    }
    if (!smokeTestTimedQueueRoundTrip()) {
        setStatus("MULTIPLAYER SMOKE TEST FAILED: TIMED QUEUE ROUND TRIP");
        return false;
    }

    CharacterCreationSheet sheet;
    sheet.playerId = launchOptions.mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    sheet.name = launchOptions.mode == NetworkLaunchMode::Host ? "Smoke Host" : "Smoke Guest";
    sheet.primaryStats = { 5, 5, 5, 5, 5, 5, 10 };
    sheet.taggedSkills = { SKILL_SMALL_GUNS, SKILL_FIRST_AID, SKILL_SPEECH };

    bool submitted = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    while (std::chrono::steady_clock::now() < deadline) {
        networkRuntimeBackgroundProcess();
        if (lobbyStarted && !submitted && lobby.state() == NetworkLobbyState::Waiting) {
            pendingLocalSheet = sheet;
            CharacterLobbyError error = lobby.submitLocalSheet(sheet);
            if (error != CharacterLobbyError::None) {
                setStatus(std::string("MULTIPLAYER SMOKE TEST FAILED: ") + characterLobbyErrorMessage(error));
                break;
            }
            submitted = true;
            reportLobbyStatus();
        }

        if (lobbyStarted && lobby.state() == NetworkLobbyState::Ready) {
            const CharacterCreationSheet* peer = lobby.peerSheet();
            if (peer == nullptr) {
                break;
            }

            EngineExecutionProbeCounts authorityProbeCounts;
            if (!networkWorldEnter(launchOptions.mode, sheet, *peer)) {
                setStatus("MULTIPLAYER SMOKE TEST FAILED: ENGINE WORLD ENTRY");
                break;
            }
            if (!networkWorldRunSharedModalSmokeTest()) {
                setStatus("MULTIPLAYER SMOKE TEST FAILED: SHARED MODAL CONTROLLER");
                break;
            }
            if (!networkWorldRunPartyExperienceSmokeTest()) {
                setStatus("MULTIPLAYER SMOKE TEST FAILED: PARTY EXPERIENCE AUTHORITY");
                break;
            }

            const SessionId sessionId = bootstrap.sessionId();
            std::uint64_t nextSendSequence = lobby.nextSendSequence();
            std::uint64_t nextReceiveSequence = lobby.nextReceiveSequence();
            std::unique_ptr<Transport> transport = lobby.takeTransport();
            if (transport == nullptr) {
                setStatus("MULTIPLAYER SMOKE TEST FAILED: MISSING GAMEPLAY TRANSPORT");
                break;
            }

            GameCommand scenarioCommand;
            scenarioCommand.sequence = CommandSequence { 1 };
            scenarioCommand.playerId = kGuestPlayerId;
            scenarioCommand.actorId = EntityId { 2 };
            scenarioCommand.expectedPhase = SessionPhase::Exploration;
            scenarioCommand.expectedPhaseRevision = networkWorldPhaseRevision();
            bool scenarioCommandReady = true;
            Object* scenarioActor = networkWorldPlayerActor(kGuestPlayerId);
            int scenarioStartingTile = scenarioActor != nullptr ? scenarioActor->tile : -1;
            int scenarioDestinationTile = -1;
            std::optional<EntityId> scenarioTargetId;
            std::optional<QuestSmokeFixture> questFixture;
            std::optional<ElevatorSmokeFixture> elevatorFixture;
            if (smokeScenario == SmokeScenario::Door) {
                scenarioTargetId = networkWorldPrepareDoorSmokeTest();
                scenarioStartingTile = scenarioActor != nullptr ? scenarioActor->tile : -1;
            } else if (smokeScenario == SmokeScenario::Pickup) {
                scenarioTargetId = networkWorldPreparePickupSmokeTest();
                scenarioStartingTile = scenarioActor != nullptr ? scenarioActor->tile : -1;
            } else if (smokeScenario == SmokeScenario::Loot) {
                scenarioTargetId = networkWorldPrepareLootSmokeTest();
                scenarioStartingTile = scenarioActor != nullptr ? scenarioActor->tile : -1;
                if (scenarioTargetId.has_value()
                    && !networkWorldVerifyLootRangeSmokeTest(*scenarioTargetId)) {
                    scenarioTargetId.reset();
                }
            } else if (smokeScenario == SmokeScenario::PlayerTransfer) {
                scenarioTargetId = networkWorldPreparePlayerTransferSmokeTest();
                scenarioStartingTile = scenarioActor != nullptr ? scenarioActor->tile : -1;
                if (scenarioTargetId.has_value()
                    && !networkWorldVerifyPlayerTransferRangeSmokeTest(*scenarioTargetId)) {
                    scenarioTargetId.reset();
                }
            } else if (smokeScenario == SmokeScenario::Skill) {
                scenarioTargetId = networkWorldPrepareSkillSmokeTest();
                scenarioStartingTile = scenarioActor != nullptr ? scenarioActor->tile : -1;
            } else if (smokeScenario == SmokeScenario::Scenery) {
                scenarioTargetId = networkWorldPrepareScenerySmokeTest();
                scenarioStartingTile = scenarioActor != nullptr ? scenarioActor->tile : -1;
            } else if (smokeScenario == SmokeScenario::Container) {
                scenarioTargetId = networkWorldPrepareContainerSmokeTest();
                scenarioStartingTile = scenarioActor != nullptr ? scenarioActor->tile : -1;
            } else if (smokeScenario == SmokeScenario::Quest) {
                questFixture = networkWorldPrepareQuestSmokeTest();
                if (questFixture.has_value()) {
                    scenarioTargetId = questFixture->targetId;
                }
                scenarioStartingTile = scenarioActor != nullptr ? scenarioActor->tile : -1;
            } else if (smokeScenario == SmokeScenario::Elevation) {
                elevatorFixture = networkWorldPrepareElevatorSmokeTest();
                scenarioStartingTile = scenarioActor != nullptr ? scenarioActor->tile : -1;
            } else if (scenarioActor != nullptr) {
                for (int distance = 1; distance <= 4 && scenarioDestinationTile == -1; distance++) {
                    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
                        int candidate = tile_num_in_direction(scenarioStartingTile, rotation, distance);
                        if (hexGridTileIsValid(candidate)
                            && make_path(scenarioActor, scenarioStartingTile, candidate, nullptr, 1) > 0) {
                            scenarioDestinationTile = candidate;
                            break;
                        }
                    }
                }
            }
            if ((smokeScenario != SmokeScenario::Movement
                    && smokeScenario != SmokeScenario::Elevation
                    && !scenarioTargetId.has_value())
                || (smokeScenario == SmokeScenario::Elevation && !elevatorFixture.has_value())
                || (smokeScenario == SmokeScenario::Movement && scenarioDestinationTile == -1)) {
                setStatus("MULTIPLAYER SMOKE TEST FAILED: NO EXPLORATION FIXTURE");
                break;
            }
            switch (smokeScenario) {
            case SmokeScenario::Movement:
                scenarioCommand.payload = MoveCommand { scenarioDestinationTile, scenarioActor->elevation, false };
                break;
            case SmokeScenario::Door:
                scenarioCommand.payload = InteractCommand { *scenarioTargetId };
                break;
            case SmokeScenario::Pickup:
                scenarioCommand.payload = PickupCommand { *scenarioTargetId };
                break;
            case SmokeScenario::Loot:
                scenarioCommand.payload = LootCommand { *scenarioTargetId };
                break;
            case SmokeScenario::PlayerTransfer: {
                Object* item = networkWorldFindObject(*scenarioTargetId);
                ItemDescriptor descriptor;
                if (item == nullptr || !networkWorldDescribeItem(item, descriptor)) {
                    setStatus("MULTIPLAYER SMOKE TEST FAILED: INVALID PLAYER TRANSFER FIXTURE");
                    scenarioCommandReady = false;
                    break;
                }
                scenarioCommand.payload = InventoryTransferCommand {
                    EntityId { kGuestPlayerId.value },
                    EntityId { kHostPlayerId.value },
                    *scenarioTargetId,
                    3,
                    7,
                    descriptor,
                };
                break;
            }
            case SmokeScenario::Skill:
                scenarioCommand.payload = UseSkillCommand { *scenarioTargetId, ExplorationSkill::Traps };
                break;
            case SmokeScenario::Scenery:
                scenarioCommand.payload = UseSkillCommand { *scenarioTargetId, ExplorationSkill::Science };
                break;
            case SmokeScenario::Container:
                scenarioCommand.payload = UseSkillCommand { *scenarioTargetId, ExplorationSkill::Lockpick };
                break;
            case SmokeScenario::Quest:
                scenarioCommand.payload = UseItemOnCommand { questFixture->itemId, questFixture->targetId };
                break;
            case SmokeScenario::Elevation:
                scenarioCommand.payload = ElevatorCommand { elevatorFixture->elevatorType, elevatorFixture->destinationLevel };
                break;
            }
            if (!scenarioCommandReady) {
                break;
            }
            EventSequence scenarioFinalEventSequence {
                smokeScenario == SmokeScenario::Pickup ? 2ULL : 1ULL
            };

            bool gameplayPassed = false;
            std::vector<GameEvent> authoritativeEvents;
            if (smokeScenario == SmokeScenario::Quest) {
                engineExecutionProbeBegin();
            }
            if (launchOptions.mode == NetworkLaunchMode::Join) {
                ProtocolEnvelope commandEnvelope;
                commandEnvelope.sessionId = sessionId;
                commandEnvelope.sequence = nextSendSequence++;
                std::vector<std::uint8_t> commandPacket;
                if (encodeGameCommand(scenarioCommand, commandEnvelope) != GameplayWireError::None
                    || encodeEnvelope(commandEnvelope, commandPacket) != ProtocolError::None
                    || transport->send(std::move(commandPacket)) != TransportSendResult::Sent) {
                    setStatus("MULTIPLAYER SMOKE TEST FAILED: COULD NOT SEND SCENARIO COMMAND");
                    break;
                }

                bool receivedResult = false;
                std::uint64_t receivedEventCount = 0;
                bool stateConverged = false;
                while (std::chrono::steady_clock::now() < deadline
                    && (!receivedResult || receivedEventCount < scenarioFinalEventSequence.value || !stateConverged)) {
                    transport->poll();
                    while (std::optional<Packet> packet = transport->receive()) {
                        ProtocolDecodeResult decoded = decodeEnvelope(*packet);
                        if (!decoded || decoded.envelope.sessionId != sessionId
                            || decoded.envelope.sequence != nextReceiveSequence++) {
                            setStatus("MULTIPLAYER SMOKE TEST FAILED: INVALID GAMEPLAY ENVELOPE");
                            break;
                        }
                        if (decoded.envelope.kind == MessageKind::CommandResult) {
                            CommandResultDecodeResult result = decodeCommandResult(decoded.envelope);
                            receivedResult = result
                                && result.result.commandSequence == scenarioCommand.sequence
                                && result.result.status == CommandStatus::Accepted
                                && result.result.firstEventSequence == EventSequence { 1 }
                                && result.result.eventCount == 1;
                        } else if (decoded.envelope.kind == MessageKind::Event) {
                            GameEventDecodeResult event = decodeGameEvent(decoded.envelope);
                            bool eventApplied = false;
                            EventSequence expectedEventSequence { receivedEventCount + 1 };
                            if (event
                                && event.event.sequence == expectedEventSequence
                                && event.event.causedBy == scenarioCommand.sequence
                                && smokeScenario == SmokeScenario::Door) {
                                const auto* door = event ? std::get_if<DoorUseStartedEvent>(&event.event.payload) : nullptr;
                                eventApplied = door != nullptr
                                    && door->actorId == scenarioCommand.actorId
                                    && door->targetId == *scenarioTargetId
                                    && networkWorldApplyPeerDoorUse(*door);
                            } else if (event && event.event.sequence == expectedEventSequence
                                && event.event.causedBy == scenarioCommand.sequence
                                && smokeScenario == SmokeScenario::Movement) {
                                const auto* movement = event ? std::get_if<ActorMovementStartedEvent>(&event.event.payload) : nullptr;
                                eventApplied = movement != nullptr
                                    && movement->actorId == scenarioCommand.actorId
                                    && movement->startingTile == scenarioStartingTile
                                    && movement->destinationTile == scenarioDestinationTile
                                    && movement->elevation == scenarioActor->elevation
                                    && !movement->running
                                    && !movement->path.empty()
                                    && networkWorldApplyPeerMove(*movement);
                            } else if (event && event.event.sequence == expectedEventSequence
                                && event.event.causedBy == scenarioCommand.sequence
                                && smokeScenario == SmokeScenario::Pickup) {
                                if (receivedEventCount == 0) {
                                    const auto* pickup = std::get_if<ItemPickupStartedEvent>(&event.event.payload);
                                    eventApplied = pickup != nullptr
                                        && pickup->actorId == scenarioCommand.actorId
                                        && pickup->targetId == *scenarioTargetId
                                        && networkWorldApplyPeerPickup(*pickup);
                                } else {
                                    const auto* pickup = std::get_if<ItemPickupCompletedEvent>(&event.event.payload);
                                    eventApplied = pickup != nullptr
                                        && pickup->actorId == scenarioCommand.actorId
                                        && pickup->targetId == *scenarioTargetId
                                        && pickup->succeeded
                                        && pickup->quantity > 0
                                        && networkWorldApplyPeerPickupCompletion(*pickup);
                                }
                            } else if (event && event.event.sequence == expectedEventSequence
                                && event.event.causedBy == scenarioCommand.sequence
                                && smokeScenario == SmokeScenario::Loot) {
                                const auto* loot = std::get_if<LootStartedEvent>(&event.event.payload);
                                eventApplied = loot != nullptr
                                    && loot->actorId == scenarioCommand.actorId
                                    && loot->targetId == *scenarioTargetId
                                    && networkWorldApplyPeerLoot(*loot);
                            } else if (event && event.event.sequence == expectedEventSequence
                                && event.event.causedBy == scenarioCommand.sequence
                                && smokeScenario == SmokeScenario::PlayerTransfer) {
                                const auto* transfer = std::get_if<InventoryTransferredEvent>(&event.event.payload);
                                eventApplied = transfer != nullptr
                                    && transfer->actorId == scenarioCommand.actorId
                                    && transfer->sourceId == EntityId { kGuestPlayerId.value }
                                    && transfer->destinationId == EntityId { kHostPlayerId.value }
                                    && transfer->itemId == *scenarioTargetId
                                    && transfer->quantity == 3
                                    && transfer->sourceQuantity == 7
                                    && isValid(transfer->remainderItemId)
                                    && transfer->itemDescriptor.pid == PROTO_ID_MONEY
                                    && networkWorldApplyInventoryTransfer(*transfer)
                                    && item_caps_total(networkWorldPlayerActor(kHostPlayerId)) == 3
                                    && item_caps_total(networkWorldPlayerActor(kGuestPlayerId)) == 4;
                            } else if (event && event.event.sequence == expectedEventSequence
                                && event.event.causedBy == scenarioCommand.sequence
                                && smokeScenario == SmokeScenario::Skill) {
                                const auto* skill = std::get_if<SkillUseStartedEvent>(&event.event.payload);
                                eventApplied = skill != nullptr
                                    && skill->actorId == scenarioCommand.actorId
                                    && skill->targetId == *scenarioTargetId
                                    && skill->skill == ExplorationSkill::Traps
                                && networkWorldApplyPeerSkillUse(*skill);
                            } else if (event && event.event.sequence == expectedEventSequence
                                && event.event.causedBy == scenarioCommand.sequence
                                && smokeScenario == SmokeScenario::Container) {
                                const auto* skill = std::get_if<SkillUseStartedEvent>(&event.event.payload);
                                eventApplied = skill != nullptr
                                    && skill->actorId == scenarioCommand.actorId
                                    && skill->targetId == *scenarioTargetId
                                    && skill->skill == ExplorationSkill::Lockpick
                                    && networkWorldApplyPeerSkillUse(*skill);
                            } else if (event && event.event.sequence == expectedEventSequence
                                && event.event.causedBy == scenarioCommand.sequence
                                && smokeScenario == SmokeScenario::Scenery) {
                                const auto* skill = std::get_if<SkillUseStartedEvent>(&event.event.payload);
                                eventApplied = skill != nullptr
                                    && skill->actorId == scenarioCommand.actorId
                                    && skill->targetId == *scenarioTargetId
                                    && skill->skill == ExplorationSkill::Science
                                    && networkWorldApplyPeerSkillUse(*skill);
                            } else if (event && event.event.sequence == expectedEventSequence
                                && event.event.causedBy == scenarioCommand.sequence
                                && smokeScenario == SmokeScenario::Quest) {
                                const auto* itemUse = std::get_if<ItemUseStartedEvent>(&event.event.payload);
                                eventApplied = itemUse != nullptr
                                    && itemUse->actorId == scenarioCommand.actorId
                                    && itemUse->itemId == questFixture->itemId
                                    && itemUse->targetId == questFixture->targetId
                                    && networkWorldApplyPeerItemUse(*itemUse);
                            } else if (event && event.event.sequence == expectedEventSequence
                                && event.event.causedBy == scenarioCommand.sequence
                                && smokeScenario == SmokeScenario::Elevation) {
                                const auto* elevator = std::get_if<ElevatorTransitionedEvent>(&event.event.payload);
                                eventApplied = elevator != nullptr
                                    && elevator->actorId == scenarioCommand.actorId
                                    && elevator->elevatorType == elevatorFixture->elevatorType
                                    && elevator->hostElevation == elevatorFixture->sourceElevation
                                    && elevator->hostTile == elevatorFixture->hostTile
                                    && elevator->guestElevation == elevatorFixture->destinationElevation
                                    && networkWorldApplyPeerElevator(*elevator);
                            }
                            if (eventApplied) {
                                receivedEventCount++;
                            }
                        } else if (decoded.envelope.kind == MessageKind::Combat) {
                            SnapshotDecodeResult state = decodeSnapshot(decoded.envelope.payload);
                            WorldSnapshot localState;
                            anim_stop();
                            bool boundaryMatches = state
                                && state.snapshot.lastIncludedEvent == scenarioFinalEventSequence;
                            bool stateApplied = boundaryMatches
                                && networkWorldApplyAuthoritativeState(state.snapshot);
                            bool localCaptured = stateApplied
                                && networkWorldCaptureAuthoritativeState(scenarioFinalEventSequence, localState);
                            stateConverged = localCaptured;
                            if (localCaptured) {
                                SnapshotDigestResult expectedDigest = computeSnapshotDigest(state.snapshot);
                                SnapshotDigestResult actualDigest = computeSnapshotDigest(localState);
                                stateConverged = expectedDigest && actualDigest
                                    && expectedDigest.digest == actualDigest.digest;
                                if (stateConverged && smokeScenario == SmokeScenario::Quest) {
                                    stateConverged = networkWorldVerifyQuestSmokeTest(*questFixture);
                                } else if (stateConverged && smokeScenario == SmokeScenario::Elevation) {
                                    stateConverged = networkWorldVerifyElevatorSmokeTest(*elevatorFixture);
                                }
                            }
                            if (!stateConverged) {
                                std::fprintf(stderr,
                                    "Multiplayer scenario checkpoint: decoded=%d boundary=%d applied=%d captured=%d.\n",
                                    state ? 1 : 0,
                                    boundaryMatches ? 1 : 0,
                                    stateApplied ? 1 : 0,
                                    localCaptured ? 1 : 0);
                                if (state && localCaptured) {
                                    SnapshotDigestResult expectedDigest = computeSnapshotDigest(state.snapshot);
                                    SnapshotDigestResult actualDigest = computeSnapshotDigest(localState);
                                    std::fprintf(stderr,
                                        "Multiplayer scenario divergence: section=%d expected=%llu actual=%llu.\n",
                                        static_cast<int>(firstDivergentSection(expectedDigest.digest, actualDigest.digest)),
                                        static_cast<unsigned long long>(expectedDigest.digest.overall),
                                        static_cast<unsigned long long>(actualDigest.digest.overall));
                                }
                            }
                        } else {
                            setStatus("MULTIPLAYER SMOKE TEST FAILED: UNEXPECTED GAMEPLAY MESSAGE");
                            break;
                        }
                    }
                    if (!transport->isConnected()
                        && (!receivedResult || receivedEventCount < scenarioFinalEventSequence.value || !stateConverged)) {
                        setStatus("MULTIPLAYER SMOKE TEST FAILED: GAMEPLAY TRANSPORT DISCONNECTED");
                        break;
                    }
                    if (!receivedResult || receivedEventCount < scenarioFinalEventSequence.value || !stateConverged) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
                gameplayPassed = receivedResult
                    && receivedEventCount == scenarioFinalEventSequence.value
                    && stateConverged;
            } else {
                std::optional<GameCommand> receivedCommand;
                while (std::chrono::steady_clock::now() < deadline && !receivedCommand.has_value()) {
                    transport->poll();
                    while (std::optional<Packet> packet = transport->receive()) {
                        ProtocolDecodeResult decoded = decodeEnvelope(*packet);
                        if (!decoded || decoded.envelope.sessionId != sessionId
                            || decoded.envelope.sequence != nextReceiveSequence++) {
                            setStatus("MULTIPLAYER SMOKE TEST FAILED: INVALID GAMEPLAY ENVELOPE");
                            break;
                        }
                        GameCommandDecodeResult command = decodeGameCommand(decoded.envelope);
                        bool payloadMatches = false;
                        if (command) {
                            if (smokeScenario == SmokeScenario::Door) {
                                const auto* door = std::get_if<InteractCommand>(&command.command.payload);
                                payloadMatches = door != nullptr && door->targetId == *scenarioTargetId;
                            } else if (smokeScenario == SmokeScenario::Pickup) {
                                const auto* pickup = std::get_if<PickupCommand>(&command.command.payload);
                                payloadMatches = pickup != nullptr && pickup->targetId == *scenarioTargetId;
                            } else if (smokeScenario == SmokeScenario::Loot) {
                                const auto* loot = std::get_if<LootCommand>(&command.command.payload);
                                payloadMatches = loot != nullptr && loot->targetId == *scenarioTargetId;
                            } else if (smokeScenario == SmokeScenario::PlayerTransfer) {
                                const auto* transfer = std::get_if<InventoryTransferCommand>(&command.command.payload);
                                payloadMatches = transfer != nullptr
                                    && transfer->sourceId == EntityId { kGuestPlayerId.value }
                                    && transfer->destinationId == EntityId { kHostPlayerId.value }
                                    && transfer->itemId == *scenarioTargetId
                                    && transfer->quantity == 3
                                    && transfer->sourceQuantity == 7
                                    && transfer->itemDescriptor.pid == PROTO_ID_MONEY;
                            } else if (smokeScenario == SmokeScenario::Skill) {
                                const auto* skill = std::get_if<UseSkillCommand>(&command.command.payload);
                                payloadMatches = skill != nullptr
                                    && skill->targetId == *scenarioTargetId
                                    && skill->skill == ExplorationSkill::Traps;
                            } else if (smokeScenario == SmokeScenario::Scenery) {
                                const auto* skill = std::get_if<UseSkillCommand>(&command.command.payload);
                                payloadMatches = skill != nullptr
                                    && skill->targetId == *scenarioTargetId
                                    && skill->skill == ExplorationSkill::Science;
                            } else if (smokeScenario == SmokeScenario::Container) {
                                const auto* skill = std::get_if<UseSkillCommand>(&command.command.payload);
                                payloadMatches = skill != nullptr
                                    && skill->targetId == *scenarioTargetId
                                    && skill->skill == ExplorationSkill::Lockpick;
                            } else if (smokeScenario == SmokeScenario::Quest) {
                                const auto* itemUse = std::get_if<UseItemOnCommand>(&command.command.payload);
                                payloadMatches = itemUse != nullptr
                                    && itemUse->itemId == questFixture->itemId
                                    && itemUse->targetId == questFixture->targetId;
                            } else if (smokeScenario == SmokeScenario::Elevation) {
                                const auto* elevator = std::get_if<ElevatorCommand>(&command.command.payload);
                                payloadMatches = elevator != nullptr
                                    && elevator->elevatorType == elevatorFixture->elevatorType
                                    && elevator->destinationLevel == elevatorFixture->destinationLevel;
                            } else {
                                const auto* movement = std::get_if<MoveCommand>(&command.command.payload);
                                payloadMatches = movement != nullptr
                                    && movement->destinationTile == scenarioDestinationTile
                                    && movement->elevation == scenarioActor->elevation
                                    && !movement->running;
                            }
                        }
                        if (!payloadMatches || command.command.sequence != scenarioCommand.sequence
                            || command.command.playerId != scenarioCommand.playerId
                            || command.command.actorId != scenarioCommand.actorId
                            || command.command.expectedPhase != scenarioCommand.expectedPhase
                            || command.command.expectedPhaseRevision != scenarioCommand.expectedPhaseRevision) {
                            setStatus("MULTIPLAYER SMOKE TEST FAILED: INVALID SCENARIO COMMAND");
                            break;
                        }
                        receivedCommand = command.command;
                    }
                    if (!transport->isConnected() && !receivedCommand.has_value()) {
                        setStatus("MULTIPLAYER SMOKE TEST FAILED: GAMEPLAY TRANSPORT DISCONNECTED");
                        break;
                    }
                    if (!receivedCommand.has_value()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }

                if (receivedCommand.has_value()) {
                    AuthoritativeCommandResult outcome = networkWorldProcessCommand(*receivedCommand);
                    Object* scenarioTarget = scenarioTargetId.has_value()
                        ? networkWorldFindObject(*scenarioTargetId)
                        : nullptr;
                    std::optional<GameEvent> pickupCompletion;
                    auto actionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
                    auto actionComplete = [&]() {
                        switch (smokeScenario) {
                        case SmokeScenario::Movement:
                            return scenarioActor->tile == scenarioDestinationTile;
                        case SmokeScenario::Door:
                            return anim_busy(scenarioActor) != -1 && anim_busy(scenarioTarget) != -1;
                        case SmokeScenario::Pickup:
                            return pickupCompletion.has_value();
                        case SmokeScenario::Loot:
                            return true;
                        case SmokeScenario::PlayerTransfer:
                            return item_caps_total(networkWorldPlayerActor(kHostPlayerId)) == 3
                                && item_caps_total(networkWorldPlayerActor(kGuestPlayerId)) == 4;
                        case SmokeScenario::Skill:
                            return anim_busy(scenarioActor) != -1;
                        case SmokeScenario::Scenery:
                            return anim_busy(scenarioActor) != -1;
                        case SmokeScenario::Container:
                            return anim_busy(scenarioActor) != -1;
                        case SmokeScenario::Quest:
                            return anim_busy(scenarioActor) != -1
                                && networkWorldFindObject(questFixture->itemId) == nullptr;
                        case SmokeScenario::Elevation:
                            return networkWorldVerifyElevatorSmokeTest(*elevatorFixture);
                        }
                        return false;
                    };
                    while (!actionComplete() && std::chrono::steady_clock::now() < actionDeadline) {
                        object_animate();
                        if (smokeScenario == SmokeScenario::Pickup) {
                            while (std::optional<GameEvent> deferred = networkWorldTakeDeferredEvent()) {
                                if (std::holds_alternative<ItemPickupCompletedEvent>(deferred->payload)) {
                                    pickupCompletion = std::move(*deferred);
                                    break;
                                }
                            }
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    bool objectMutated = (smokeScenario != SmokeScenario::Scenery
                                             || networkWorldMutateScenerySmokeTest(*scenarioTargetId))
                        && (smokeScenario != SmokeScenario::Container
                            || networkWorldMutateContainerSmokeTest(*scenarioTargetId));
                    if (!objectMutated) {
                        setStatus("MULTIPLAYER SMOKE TEST FAILED: OBJECT MUTATION FIXTURE");
                    }
                    if (smokeScenario == SmokeScenario::Door && outcome.event.has_value()) {
                        auto* door = std::get_if<DoorUseStartedEvent>(&outcome.event->payload);
                        if (door != nullptr && scenarioTarget != nullptr) {
                            door->open = obj_is_open(scenarioTarget) != 0;
                            door->locked = obj_is_locked(scenarioTarget);
                            door->frame = scenarioTarget->frame;
                        }
                    }
                    if (outcome.event.has_value()) {
                        authoritativeEvents.push_back(*outcome.event);
                    }
                    if (pickupCompletion.has_value()) {
                        pickupCompletion->sequence = scenarioFinalEventSequence;
                        authoritativeEvents.push_back(std::move(*pickupCompletion));
                    }
                    WorldSnapshot state;
                    std::vector<std::uint8_t> statePayload;
                    anim_stop();
                    bool accepted = outcome.result.status == CommandStatus::Accepted;
                    bool scenarioCompleted = actionComplete();
                    bool questCompleted = smokeScenario != SmokeScenario::Quest
                        || networkWorldVerifyQuestSmokeTest(*questFixture);
                    bool elevationCompleted = smokeScenario != SmokeScenario::Elevation
                        || networkWorldVerifyElevatorSmokeTest(*elevatorFixture);
                    bool captured = accepted
                        && scenarioCompleted
                        && questCompleted
                        && elevationCompleted
                        && objectMutated
                        && authoritativeEvents.size() == scenarioFinalEventSequence.value
                        && networkWorldCaptureAuthoritativeState(scenarioFinalEventSequence, state);
                    SnapshotError snapshotError = captured
                        ? encodeSnapshot(state, statePayload)
                        : SnapshotError::None;
                    bool validOutcome = captured && snapshotError == SnapshotError::None;
                    if (!validOutcome) {
                        std::fprintf(stderr,
                            "Multiplayer scenario: accepted=%d event=%d action=%d captured=%d snapshot=%d rejection=%d.\n",
                            accepted ? 1 : 0,
                            outcome.event.has_value() ? 1 : 0,
                            scenarioCompleted ? 1 : 0,
                            captured ? 1 : 0,
                            captured ? static_cast<int>(snapshotError) : -1,
                            static_cast<int>(outcome.result.rejection));
                    }

                    ProtocolEnvelope resultEnvelope;
                    resultEnvelope.sessionId = sessionId;
                    resultEnvelope.sequence = nextSendSequence++;
                    std::vector<std::uint8_t> resultPacket;
                    gameplayPassed = validOutcome
                        && encodeCommandResult(outcome.result, resultEnvelope) == GameplayWireError::None
                        && encodeEnvelope(resultEnvelope, resultPacket) == ProtocolError::None
                        && transport->send(std::move(resultPacket)) == TransportSendResult::Sent;
                    for (const GameEvent& event : authoritativeEvents) {
                        ProtocolEnvelope eventEnvelope;
                        eventEnvelope.sessionId = sessionId;
                        eventEnvelope.sequence = nextSendSequence++;
                        std::vector<std::uint8_t> eventPacket;
                        gameplayPassed = gameplayPassed
                            && encodeGameEvent(event, eventEnvelope) == GameplayWireError::None
                            && encodeEnvelope(eventEnvelope, eventPacket) == ProtocolError::None
                            && transport->send(std::move(eventPacket)) == TransportSendResult::Sent;
                    }
                    ProtocolEnvelope stateEnvelope;
                    stateEnvelope.kind = MessageKind::Combat;
                    stateEnvelope.sessionId = sessionId;
                    stateEnvelope.sequence = nextSendSequence++;
                    stateEnvelope.payload = std::move(statePayload);
                    std::vector<std::uint8_t> statePacket;
                    gameplayPassed = gameplayPassed
                        && encodeEnvelope(stateEnvelope, statePacket) == ProtocolError::None
                        && transport->send(std::move(statePacket)) == TransportSendResult::Sent;
                    if (gameplayPassed) {
                        // Give the non-blocking socket a chance to flush before this test process exits.
                        for (int attempt = 0; attempt < 50 && transport->isConnected(); attempt++) {
                            transport->poll();
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                    }
                }
            }

            EngineExecutionProbeCounts scenarioProbeCounts;
            bool scenarioAuthorityPassed = true;
            if (smokeScenario == SmokeScenario::Quest) {
                scenarioProbeCounts = engineExecutionProbeEnd();
                scenarioAuthorityPassed = launchOptions.mode == NetworkLaunchMode::Host
                    ? scenarioProbeCounts.scriptProcedures > 0 && scenarioProbeCounts.combatAttacks == 0
                    : scenarioProbeCounts.scriptProcedures == 0
                        && scenarioProbeCounts.combatAttacks == 0
                        && scenarioProbeCounts.randomDraws == 0;
            }

            if (!gameplayPassed || !scenarioAuthorityPassed) {
                if (runtimeStatus.find("SMOKE TEST FAILED") == std::string::npos) {
                    setStatus(!scenarioAuthorityPassed
                            ? "MULTIPLAYER SMOKE TEST FAILED: QUEST AUTHORITY PROBE"
                            : "MULTIPLAYER SMOKE TEST FAILED: GAMEPLAY WIRE EXCHANGE");
                }
                break;
            }

            if (smokeScenario == SmokeScenario::Quest) {
                authorityProbeCounts = scenarioProbeCounts;
            } else if (smokeScenario == SmokeScenario::Elevation) {
                // The transition event itself is applied under the execution probe
                // above. The standard door/attack probe assumes both actors remain
                // on the entrance elevation, which is deliberately false here.
                authorityProbeCounts = {};
            } else if (!networkWorldRunEngineAuthoritySmokeTest(authorityProbeCounts)) {
                setStatus("MULTIPLAYER SMOKE TEST FAILED: ENGINE AUTHORITY PROBE");
                break;
            }

            transport->close();

            bool reconnectPassed = false;
            if (launchOptions.mode == NetworkLaunchMode::Join) {
                std::optional<TransportPeerIdentity> identity = bootstrap.peerIdentity();
                TcpConnectResult reconnect = identity.has_value()
                    ? connectTcp(launchOptions.address, launchOptions.port, 5000, identity)
                    : TcpConnectResult {};
                if (reconnect) {
                    reconnectPassed = sendReconnectHandshake(*reconnect.transport,
                        ReconnectHello { sessionId, kGuestPlayerId, bootstrap.reconnectToken(), EventSequence {} });
                    bool welcomed = false;
                    std::uint64_t replayedEventCount = 0;
                    while (reconnectPassed && std::chrono::steady_clock::now() < deadline
                        && (!welcomed || replayedEventCount < scenarioFinalEventSequence.value)) {
                        reconnect.transport->poll();
                        while (std::optional<Packet> packet = reconnect.transport->receive()) {
                            ProtocolDecodeResult decoded = decodeEnvelope(*packet);
                            if (!decoded || decoded.envelope.sessionId != sessionId) {
                                reconnectPassed = false;
                                break;
                            }
                            if (decoded.envelope.sequence == 1 && decoded.envelope.kind == MessageKind::Handshake) {
                                HandshakeDecodeResult handshake = decodeHandshakeMessage(decoded.envelope);
                                const ReconnectWelcome* welcome = handshake
                                    ? std::get_if<ReconnectWelcome>(&handshake.message)
                                    : nullptr;
                                welcomed = welcome != nullptr
                                    && welcome->sessionId == sessionId
                                    && welcome->latestEvent == scenarioFinalEventSequence;
                            } else if (decoded.envelope.sequence == replayedEventCount + 2
                                && decoded.envelope.kind == MessageKind::Event) {
                                GameEventDecodeResult event = decodeGameEvent(decoded.envelope);
                                if (event
                                    && event.event.sequence == EventSequence { replayedEventCount + 1 }
                                    && event.event.causedBy == scenarioCommand.sequence) {
                                    replayedEventCount++;
                                } else {
                                    reconnectPassed = false;
                                    break;
                                }
                            } else {
                                reconnectPassed = false;
                                break;
                            }
                        }
                        if (!reconnect.transport->isConnected()
                            && (!welcomed || replayedEventCount < scenarioFinalEventSequence.value)) {
                            reconnectPassed = false;
                        }
                        if (!welcomed || replayedEventCount < scenarioFinalEventSequence.value) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                    }
                    reconnectPassed = reconnectPassed
                        && welcomed
                        && replayedEventCount == scenarioFinalEventSequence.value;
                }
            } else {
                std::unique_ptr<Transport> resumed;
                while (std::chrono::steady_clock::now() < deadline && resumed == nullptr) {
                    resumed = bootstrap.acceptReconnectTransport();
                    if (resumed == nullptr) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
                std::optional<ReconnectHello> hello;
                while (resumed != nullptr && std::chrono::steady_clock::now() < deadline && !hello.has_value()) {
                    std::optional<HandshakeMessage> handshake = receiveReconnectHandshake(*resumed);
                    if (handshake.has_value()) {
                        if (const auto* decoded = std::get_if<ReconnectHello>(&*handshake)) {
                            hello = *decoded;
                        }
                    }
                    if (!hello.has_value()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
                if (resumed != nullptr
                    && hello.has_value()
                    && hello->sessionId == sessionId
                    && hello->playerId == kGuestPlayerId
                    && hello->lastAppliedEvent == EventSequence {}
                    && reconnectTokensEqual(hello->reconnectToken, bootstrap.reconnectToken())) {
                    ProtocolEnvelope eventEnvelope;
                    eventEnvelope.sessionId = sessionId;
                    reconnectPassed = sendReconnectHandshake(*resumed,
                        ReconnectWelcome { sessionId, scenarioFinalEventSequence });
                    for (std::size_t index = 0; reconnectPassed && index < authoritativeEvents.size(); index++) {
                        eventEnvelope.sequence = index + 2;
                        Packet eventPacket;
                        reconnectPassed = encodeGameEvent(authoritativeEvents[index], eventEnvelope) == GameplayWireError::None
                            && encodeEnvelope(eventEnvelope, eventPacket) == ProtocolError::None
                            && resumed->send(std::move(eventPacket)) == TransportSendResult::Sent;
                    }
                    for (int attempt = 0; reconnectPassed && attempt < 100; attempt++) {
                        resumed->poll();
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
            }

            if (!reconnectPassed) {
                setStatus("MULTIPLAYER SMOKE TEST FAILED: TLS RECONNECT AND EVENT REPLAY");
                break;
            }

            std::fprintf(stdout,
                "MULTIPLAYER_SMOKE_TEST_PASS role=%s session=%llu local=%s peer=%s command=%s checkpoint=state-digest reconnect=tls-replay xp=party scripts=%u attacks=%u rng=%u\n",
                launchOptions.mode == NetworkLaunchMode::Host ? "host" : "guest",
                static_cast<unsigned long long>(sessionId.value),
                sheet.name.c_str(),
                peer->name.c_str(),
                smokeScenarioName(),
                authorityProbeCounts.scriptProcedures,
                authorityProbeCounts.combatAttacks,
                authorityProbeCounts.randomDraws);
            std::fflush(stdout);
            return peer != nullptr;
        }
        if (bootstrap.state() == NetworkBootstrapState::Rejected
            || bootstrap.state() == NetworkBootstrapState::Failed
            || (lobbyStarted && (lobby.state() == NetworkLobbyState::Rejected || lobby.state() == NetworkLobbyState::Failed))) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::fprintf(stdout, "MULTIPLAYER_SMOKE_TEST_FAIL status=%s\n", runtimeStatus.c_str());
    std::fflush(stdout);
    return false;
}

bool networkRuntimeStart()
{
    if (launchOptions.mode == NetworkLaunchMode::Disabled) {
        return true;
    }

    return startConfiguredRuntime();
}

bool networkRuntimeHost(std::uint16_t port)
{
    networkRuntimeDisconnect();
    launchOptions.mode = NetworkLaunchMode::Host;
    launchOptions.port = port;
    launchOptions.address.clear();
    return startConfiguredRuntime();
}

bool networkRuntimeJoin(const char* endpoint)
{
    std::string address;
    std::uint16_t port;
    if (endpoint == nullptr || !parseNetworkJoinEndpoint(endpoint, address, port)) {
        setStatus("MULTIPLAYER CONNECTION FAILED: INVALID HOST ADDRESS");
        return false;
    }

    networkRuntimeDisconnect();
    launchOptions.mode = NetworkLaunchMode::Join;
    launchOptions.address = address;
    launchOptions.port = port;
    return startConfiguredRuntime();
}

void networkRuntimeDisconnect()
{
    networkRuntimeStop();
    launchOptions = NetworkLaunchOptions {};
    pendingLocalSheet.reset();
    reportedState = NetworkBootstrapState::Disabled;
    setStatus("MULTIPLAYER: NOT CONNECTED");
}

NetworkLaunchMode networkRuntimeMode()
{
    return launchOptions.mode;
}

bool networkRuntimeIsGuestReplica()
{
    return launchOptions.mode == NetworkLaunchMode::Join && networkWorldActive();
}

bool networkRuntimeConnected()
{
    return lobbyStarted
        && (lobby.state() == NetworkLobbyState::Waiting || lobby.state() == NetworkLobbyState::Ready);
}

bool networkRuntimeFailed()
{
    return bootstrap.state() == NetworkBootstrapState::Rejected
        || bootstrap.state() == NetworkBootstrapState::Failed
        || (lobbyStarted && (lobby.state() == NetworkLobbyState::Rejected || lobby.state() == NetworkLobbyState::Failed));
}

const char* networkRuntimeStatus()
{
    return runtimeStatus.empty() ? "MULTIPLAYER: NOT CONNECTED" : runtimeStatus.c_str();
}

const CharacterCreationSheet* networkRuntimeLocalSheet()
{
    return lobbyStarted ? lobby.localSheet() : nullptr;
}

const CharacterCreationSheet* networkRuntimePeerSheet()
{
    return lobbyStarted ? lobby.peerSheet() : nullptr;
}

bool networkRuntimeSendChatMessage(const char* text)
{
    return lobbyStarted && text != nullptr && lobby.sendChatMessage(text);
}

std::optional<LobbyChatMessage> networkRuntimeTakeChatMessage()
{
    return lobbyStarted ? lobby.takeChatMessage() : std::nullopt;
}

bool networkRuntimeHandleGameChatInput(int keyCode)
{
    if (keyCode != KEY_RETURN || !networkWorldActive() || !networkRuntimeConnected()) {
        return false;
    }

    char input[kMaxLobbyChatMessageLength + 2] = {};
    if (win_get_str(input,
            static_cast<int>(kMaxLobbyChatMessageLength),
            "Say to the other player:",
            80,
            80)
        != 0) {
        return true;
    }

    std::string text = trimmedChatText(input);
    if (text.empty()) {
        return true;
    }
    if (!lobby.sendChatMessage(text)) {
        char failure[] = "Multiplayer message could not be sent.";
        display_print(failure);
        return true;
    }

    PlayerId localPlayerId = launchOptions.mode == NetworkLaunchMode::Host
        ? kHostPlayerId
        : kGuestPlayerId;
    presentGameChatMessage(LobbyChatMessage { localPlayerId, std::move(text) }, "outgoing");
    return true;
}

bool networkRuntimeSubmitLocalCharacter(Object* actor)
{
    if (launchOptions.mode == NetworkLaunchMode::Disabled) {
        return true;
    }

    CharacterBuild build;
    if (!captureLegacyCharacterBuild(actor, build)) {
        setStatus("MULTIPLAYER CHARACTER REJECTED: COULD NOT READ CHARACTER BUILD");
        return false;
    }

    PlayerId playerId = launchOptions.mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    const char* actorName = critter_name(actor);
    CharacterCreationSheet sheet = characterSheetFromBuild(
        playerId,
        actorName != nullptr && *actorName != '\0' ? actorName : playerLabel(playerId),
        build);
    CharacterLobbyError error = validateCharacterSheet(sheet);
    if (error != CharacterLobbyError::None) {
        setStatus(std::string("MULTIPLAYER CHARACTER REJECTED: ") + characterLobbyErrorMessage(error));
        return false;
    }

    if (lobbyStarted && lobby.state() == NetworkLobbyState::Ready) {
        const CharacterCreationSheet* submitted = lobby.localSheet();
        return submitted != nullptr && *submitted == sheet;
    }

    pendingLocalSheet = sheet;
    if (!lobbyStarted) {
        setStatus("MULTIPLAYER: CHARACTER READY, WAITING FOR CONNECTION");
        return true;
    }

    error = lobby.submitLocalSheet(sheet);
    if (error != CharacterLobbyError::None) {
        setStatus(std::string("MULTIPLAYER CHARACTER REJECTED: ") + characterLobbyErrorMessage(error));
        return false;
    }
    reportLobbyStatus();
    return true;
}

bool networkRuntimeWaitForLobby()
{
    if (launchOptions.mode == NetworkLaunchMode::Disabled) {
        return true;
    }
    if (!pendingLocalSheet.has_value()) {
        return false;
    }

    constexpr int windowWidth = 520;
    constexpr int windowHeight = 120;
    int window = win_add((screenGetWidth() - windowWidth) / 2,
        (screenGetHeight() - windowHeight) / 2,
        windowWidth,
        windowHeight,
        colorTable[0],
        WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (window == -1) {
        return false;
    }

    std::string drawnStatus;
    bool ready = false;
    for (;;) {
        if (drawnStatus != runtimeStatus) {
            drawnStatus = runtimeStatus;
            win_fill(window, 0, 0, windowWidth, windowHeight, colorTable[0]);
            win_border(window);
            int oldFont = text_curr();
            text_font(103);
            win_print(window, "MULTIPLAYER LOBBY", 0, 20, 18, colorTable[21091]);
            text_font(101);
            win_print(window, drawnStatus.c_str(), windowWidth - 40, 20, 52, colorTable[21204]);
            win_print(window, "Press Esc to return to the main menu.", 0, 20, 88, colorTable[21204]);
            text_font(oldFont);
            win_draw(window);
        }

        if (lobbyStarted && lobby.state() == NetworkLobbyState::Ready) {
            if (launchOptions.mode == NetworkLaunchMode::Host && !lobby.startRequested()) {
                networkRuntimeRequestStart();
            }
            if (lobby.startRequested()) {
                ready = true;
                break;
            }
        }
        if ((lobbyStarted && (lobby.state() == NetworkLobbyState::Rejected || lobby.state() == NetworkLobbyState::Failed))
            || bootstrap.state() == NetworkBootstrapState::Rejected
            || bootstrap.state() == NetworkBootstrapState::Failed
            || game_user_wants_to_quit != 0) {
            break;
        }
        if (get_input() == KEY_ESCAPE) {
            break;
        }
    }

    win_delete(window);
    return ready;
}

bool networkRuntimeLobbyReady()
{
    return launchOptions.mode == NetworkLaunchMode::Disabled
        || (lobbyStarted && lobby.state() == NetworkLobbyState::Ready);
}

bool networkRuntimeRequestStart()
{
    if (launchOptions.mode != NetworkLaunchMode::Host || !lobbyStarted) {
        return false;
    }
    bool started = lobby.requestStart();
    if (started) {
        reportLobbyStatus();
    }
    return started;
}

bool networkRuntimeStartRequested()
{
    return lobbyStarted && lobby.startRequested();
}

bool networkRuntimeEnterWorld()
{
    if (launchOptions.mode == NetworkLaunchMode::Disabled) {
        return true;
    }
    const CharacterCreationSheet* local = networkRuntimeLocalSheet();
    const CharacterCreationSheet* peer = networkRuntimePeerSheet();
    if (!networkRuntimeLobbyReady()
        || !networkRuntimeStartRequested()
        || local == nullptr
        || peer == nullptr) {
        setStatus("MULTIPLAYER WORLD FAILED: LOBBY DID NOT START");
        return false;
    }
    if (!networkWorldEnter(launchOptions.mode, *local, *peer)) {
        setStatus("MULTIPLAYER WORLD FAILED: COULD NOT PLACE BOTH PLAYERS");
        return false;
    }
    lastSentLocalRotation = -1;
    nextHostCommandSequence = 1;
    nextAuthoritativeState = {};
    nextAgentWorldReport = {};
    reportLobbyStatus();
    return true;
}

bool networkRuntimeSubmitLocalMove(int destinationTile, int elevation, bool running)
{
    Object* actor = localPlayerActor();
    if (!networkWorldActive() || actor == nullptr || elevation != actor->elevation) {
        return false;
    }
    if (destinationTile == actor->tile) {
        return true;
    }

    if (launchOptions.mode == NetworkLaunchMode::Host) {
        submitHostCommand(MoveCommand { destinationTile, elevation, running });
        return true;
    }

    bool sent = lobby.sendLocalMove(destinationTile, elevation, running, -1, {}, networkWorldPhaseRevision());
    if (!sent) {
        debug_printf("Multiplayer movement command could not be sent.\n");
    }
    return true;
}

bool networkRuntimeSubmitLocalFacing(int rotation)
{
    if (!networkWorldActive() || rotation < 0 || rotation >= ROTATION_COUNT) {
        return false;
    }
    bool submitted = launchOptions.mode == NetworkLaunchMode::Host
        ? submitHostCommand(FaceCommand { rotation })
        : lobby.sendLocalFacing(rotation, networkWorldPhaseRevision());
    if (submitted) {
        lastSentLocalRotation = rotation;
    }
    return submitted;
}

bool networkRuntimeHandleLocalDoorUse(Object* target)
{
    if (!networkWorldActive() || target == nullptr || !obj_is_a_portal(target)) {
        return false;
    }

    std::optional<EntityId> targetId = networkWorldFindEntity(target);
    if (!targetId.has_value()) {
        debug_printf("Multiplayer door is missing a shared entity ID.\n");
        return true;
    }

    if (launchOptions.mode == NetworkLaunchMode::Host) {
        submitHostCommand(InteractCommand { *targetId });
        return true;
    }
    if (!lobby.sendLocalDoorUse(*targetId, networkWorldPhaseRevision())) {
        debug_printf("Multiplayer door command could not be sent.\n");
    }
    return true;
}

bool networkRuntimeHandleLocalPickup(Object* target)
{
    if (!networkWorldActive() || target == nullptr || FID_TYPE(target->fid) != OBJ_TYPE_ITEM) {
        return false;
    }

    std::optional<EntityId> targetId = networkWorldFindEntity(target);
    if (!targetId.has_value()) {
        debug_printf("Multiplayer ground item is missing a shared entity ID.\n");
        return true;
    }

    if (launchOptions.mode == NetworkLaunchMode::Host) {
        submitHostCommand(PickupCommand { *targetId });
        return true;
    }
    if (!lobby.sendLocalPickup(*targetId, networkWorldPhaseRevision())) {
        debug_printf("Multiplayer pickup command could not be sent.\n");
    }
    return true;
}

bool networkRuntimeHandleLocalLoot(Object* target)
{
    if (!networkWorldActive() || target == nullptr || FID_TYPE(target->fid) != OBJ_TYPE_CRITTER) {
        return false;
    }

    std::optional<EntityId> targetId = networkWorldFindEntity(target);
    if (!targetId.has_value()) {
        debug_printf("Multiplayer loot target is missing a shared entity ID.\n");
        return true;
    }
    if (launchOptions.mode == NetworkLaunchMode::Host) {
        submitHostCommand(LootCommand { *targetId });
        return true;
    }
    if (!lobby.sendLocalLoot(*targetId, networkWorldPhaseRevision())) {
        debug_printf("Multiplayer loot command could not be sent.\n");
    }
    return true;
}

bool networkRuntimeHandleLocalSkillUse(Object* target, int skill)
{
    ExplorationSkill explorationSkill = static_cast<ExplorationSkill>(skill);
    if (!networkWorldActive()) {
        return false;
    }
    if (target == nullptr || !isValid(explorationSkill)) {
        return true;
    }
    std::optional<EntityId> targetId = networkWorldFindEntity(target);
    if (!targetId.has_value()) {
        debug_printf("Multiplayer skill target is missing a shared entity ID.\n");
        return true;
    }
    if (launchOptions.mode == NetworkLaunchMode::Host) {
        submitHostCommand(UseSkillCommand { *targetId, explorationSkill });
        return true;
    }
    if (!lobby.sendLocalSkillUse(*targetId, explorationSkill, networkWorldPhaseRevision())) {
        debug_printf("Multiplayer skill command could not be sent.\n");
    }
    return true;
}

bool networkRuntimeHandleLocalItemUse(Object* actor, Object* item, Object* target)
{
    if (!networkWorldActive() || actor != localPlayerActor()) {
        return false;
    }
    if (networkWorldPhase() != SessionPhase::Exploration
        || item == nullptr
        || target == nullptr
        || item == target
        || FID_TYPE(item->fid) != OBJ_TYPE_ITEM
        || obj_top_environment(item) != actor) {
        debug_printf("Multiplayer item use is unavailable for this actor, item, or target.\n");
        return true;
    }
    std::optional<EntityId> itemId = networkWorldFindEntity(item);
    std::optional<EntityId> targetId = networkWorldFindEntity(target);
    if (!itemId.has_value() || !targetId.has_value()) {
        debug_printf("Multiplayer item use requires shared item and target identities.\n");
        return true;
    }
    bool submitted = launchOptions.mode == NetworkLaunchMode::Host
        ? submitHostCommand(UseItemOnCommand { *itemId, *targetId })
        : lobby.sendLocalItemUse(*itemId, *targetId, networkWorldPhaseRevision());
    if (!submitted) {
        debug_printf("Multiplayer item-use command could not be sent.\n");
    }
    return true;
}

bool networkRuntimeSubmitLocalElevator(std::int32_t elevatorType, std::int32_t destinationLevel)
{
    if (!networkWorldActive()
        || networkWorldPhase() != SessionPhase::Exploration
        || elevatorType < 0
        || elevatorType >= 12
        || destinationLevel < 0
        || destinationLevel >= 4) {
        return false;
    }
    return launchOptions.mode == NetworkLaunchMode::Host
        ? submitHostCommand(ElevatorCommand { elevatorType, destinationLevel })
        : lobby.sendLocalElevator(elevatorType, destinationLevel, networkWorldPhaseRevision());
}

bool networkRuntimeHandleLocalAttack(Object* target, int hitMode, int hitLocation)
{
    if (!networkWorldActive()
        || target == nullptr
        || FID_TYPE(target->fid) != OBJ_TYPE_CRITTER) {
        return false;
    }
    (void)hitMode;
    (void)hitLocation;
    debug_printf("Multiplayer combat input is blocked until authoritative turn ownership is available.\n");
    return true;
}

bool networkRuntimeGiveItemToPlayer(EntityId destinationActorId, EntityId itemId, std::uint32_t quantity)
{
    if (!networkWorldActive()
        || networkWorldPhase() != SessionPhase::Exploration
        || !isValid(destinationActorId)
        || !isValid(itemId)
        || quantity == 0) {
        return false;
    }

    Object* actor = localPlayerActor();
    Object* destination = networkWorldFindObject(destinationActorId);
    Object* item = networkWorldFindObject(itemId);
    Object* hostActor = networkWorldPlayerActor(kHostPlayerId);
    Object* guestActor = networkWorldPlayerActor(kGuestPlayerId);
    std::optional<EntityId> actorId = networkWorldFindEntity(actor);
    int availableQuantity = actor != nullptr && item != nullptr
        ? item_count(actor, item)
        : 0;
    ItemDescriptor itemDescriptor;
    if (actor == nullptr
        || destination == nullptr
        || destination == actor
        || (destination != hostActor && destination != guestActor)
        || actor->elevation != destination->elevation
        || obj_dist(actor, destination) != 1
        || item == nullptr
        || item->owner != actor
        || !actorId.has_value()
        || quantity > static_cast<std::uint32_t>(std::max(availableQuantity, 0))
        || !networkWorldDescribeItem(item, itemDescriptor)
        || (item->flags & (OBJECT_EQUIPPED | OBJECT_USED)) != 0) {
        return false;
    }

    InventoryTransferCommand command {
        *actorId,
        destinationActorId,
        itemId,
        quantity,
        static_cast<std::uint32_t>(availableQuantity),
        itemDescriptor,
    };
    return launchOptions.mode == NetworkLaunchMode::Host
        ? submitHostCommand(command)
        : lobby.sendLocalInventoryTransfer(command.sourceId,
              command.destinationId,
              command.itemId,
              command.quantity,
              command.sourceQuantity,
              networkWorldPhaseRevision(),
              {},
              command.itemDescriptor);
}

bool networkRuntimeRequestSharedModal(SharedModalKind kind, bool open)
{
    if (!networkWorldActive() || !isValid(kind)) {
        return false;
    }
    if ((open && networkWorldPhase() != SessionPhase::Exploration)
        || (!open && networkWorldPhase() != sharedModalPhase(kind))) {
        return false;
    }
    return launchOptions.mode == NetworkLaunchMode::Host
        ? submitHostCommand(SharedModalCommand { kind, open })
        : lobby.sendLocalSharedModal(kind, open, networkWorldPhaseRevision());
}

bool networkRuntimeBlockUnsupportedSharedModal(SharedModalKind kind)
{
    if (!networkWorldActive() || !isValid(kind)) {
        return false;
    }

    const char* name = "unknown";
    switch (kind) {
    case SharedModalKind::Dialogue:
        name = "dialogue";
        break;
    case SharedModalKind::Barter:
        name = "barter";
        break;
    case SharedModalKind::Rest:
        name = "rest";
        break;
    case SharedModalKind::Elevator:
        name = "elevator";
        break;
    case SharedModalKind::WorldMap:
        name = "world-map travel";
        break;
    }
    debug_printf("Multiplayer %s is blocked until its effects are host-authoritative.\n", name);
    return true;
}

bool networkRuntimeWorldPaused()
{
    return networkWorldActive() && networkWorldSharedModalActive();
}

bool networkRuntimeHandleLocalLootTargetChange(Object* target)
{
    if (!networkWorldActive() || target == nullptr || FID_TYPE(target->fid) != OBJ_TYPE_CRITTER) {
        return false;
    }

    std::optional<EntityId> targetId = networkWorldFindEntity(target);
    if (!targetId.has_value()) {
        debug_printf("Multiplayer loot target is missing a shared entity ID.\n");
        return true;
    }
    if (launchOptions.mode == NetworkLaunchMode::Host) {
        submitHostCommand(LootCommand { *targetId });
        return true;
    }
    if (!lobby.sendLocalLoot(*targetId, networkWorldPhaseRevision())) {
        debug_printf("Multiplayer loot target change could not be sent.\n");
    }
    return true;
}

bool networkRuntimeHandleLocalInventoryTransfer(Object* source,
    Object* destination,
    Object* item,
    std::uint32_t quantity,
    std::uint32_t sourceQuantity)
{
    if (!networkWorldActive()
        || networkWorldInventoryTransferInProgress()
        || source == nullptr
        || destination == nullptr
        || item == nullptr
        || quantity == 0) {
        return false;
    }
    if (!networkWorldIsLocalInventoryTransfer(source, destination)) {
        return false;
    }

    std::optional<EntityId> sourceId = networkWorldFindEntity(source);
    std::optional<EntityId> destinationId = networkWorldFindEntity(destination);
    std::optional<EntityId> itemId = networkWorldFindEntity(item);
    ItemDescriptor itemDescriptor;
    if (!sourceId.has_value()
        || !destinationId.has_value()
        || !itemId.has_value()
        || !networkWorldDescribeItem(item, itemDescriptor)) {
        return false;
    }

    if (launchOptions.mode != NetworkLaunchMode::Host) {
        return true;
    }
    if (!lobby.sendLocalInventoryTransfer(*sourceId,
            *destinationId,
            *itemId,
            quantity,
            sourceQuantity,
            networkWorldPhaseRevision(),
            networkWorldTakeLastItemSplit(),
            itemDescriptor)) {
        debug_printf("Multiplayer inventory transfer could not be published.\n");
    }
    return true;
}

NetworkInventoryTransferDisposition networkRuntimePrepareLocalInventoryTransfer(Object* source,
    Object* destination,
    Object* item,
    std::uint32_t quantity,
    std::uint32_t availableQuantity)
{
    if (!networkWorldActive() || networkWorldInventoryTransferInProgress()) {
        return NetworkInventoryTransferDisposition::ApplyLocally;
    }
    if (!networkWorldIsLocalInventoryTransfer(source, destination)) {
        return inven_loot_window_is_active()
            ? NetworkInventoryTransferDisposition::Reject
            : NetworkInventoryTransferDisposition::ApplyLocally;
    }
    if (quantity == 0 || quantity > availableQuantity) {
        return NetworkInventoryTransferDisposition::Reject;
    }

    std::optional<EntityId> sourceId = networkWorldFindEntity(source);
    std::optional<EntityId> destinationId = networkWorldFindEntity(destination);
    std::optional<EntityId> itemId = networkWorldFindEntity(item);
    ItemDescriptor itemDescriptor;
    if (!sourceId.has_value()
        || !destinationId.has_value()
        || !networkWorldDescribeItem(item, itemDescriptor)) {
        debug_printf("Multiplayer inventory transfer has an invalid entity.\n");
        return NetworkInventoryTransferDisposition::Reject;
    }
    if (!itemId.has_value()
        && (item->owner != source || item->data.inventory.length != 0)) {
        debug_printf("Multiplayer can only introduce a direct, empty player inventory item.\n");
        return NetworkInventoryTransferDisposition::Reject;
    }
    if (launchOptions.mode == NetworkLaunchMode::Host) {
        if (!itemId.has_value()) {
            itemId = networkWorldEnsureItemRegistered(item);
        }
        if (!itemId.has_value()) {
            debug_printf("Multiplayer host could not assign an item entity ID.\n");
            return NetworkInventoryTransferDisposition::Reject;
        }
        bool accepted = submitHostCommand(InventoryTransferCommand {
            *sourceId,
            *destinationId,
            *itemId,
            quantity,
            availableQuantity,
            itemDescriptor,
        });
        return accepted
            ? NetworkInventoryTransferDisposition::DeferToHost
            : NetworkInventoryTransferDisposition::Reject;
    }
    if (launchOptions.mode != NetworkLaunchMode::Join) {
        return NetworkInventoryTransferDisposition::Reject;
    }

    if (!lobby.sendLocalInventoryTransfer(*sourceId,
            *destinationId,
            itemId.value_or(EntityId {}),
            quantity,
            availableQuantity,
            networkWorldPhaseRevision(),
            {},
            itemDescriptor)) {
        debug_printf("Multiplayer inventory transfer command could not be sent.\n");
        return NetworkInventoryTransferDisposition::Reject;
    }
    return NetworkInventoryTransferDisposition::DeferToHost;
}

NetworkItemDropDisposition networkRuntimePrepareLocalItemDrop(Object* source,
    Object* item,
    std::uint32_t quantity,
    std::uint32_t sourceQuantity)
{
    if (!networkWorldActive() || networkWorldItemDropInProgress()) {
        return NetworkItemDropDisposition::ApplyLocally;
    }
    if (!networkWorldIsLocalItemDrop(source, item)) {
        return NetworkItemDropDisposition::ApplyLocally;
    }
    if (quantity == 0
        || quantity > sourceQuantity
        || sourceQuantity > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return NetworkItemDropDisposition::Reject;
    }

    if (launchOptions.mode == NetworkLaunchMode::Join
        && pendingLocalItemDrop.has_value()) {
        if (pendingLocalItemDrop->source == source
            && pendingLocalItemDrop->item == item
            && quantity == 1
            && pendingLocalItemDrop->remainingQuantity < sourceQuantity) {
            pendingLocalItemDrop->remainingQuantity++;
            return NetworkItemDropDisposition::DeferToHost;
        }
        debug_printf("Multiplayer item drop is waiting for host confirmation.\n");
        return NetworkItemDropDisposition::Reject;
    }

    std::optional<EntityId> sourceId = networkWorldFindEntity(source);
    std::optional<EntityId> itemId = networkWorldFindEntity(item);
    ItemDescriptor itemDescriptor;
    if (!sourceId.has_value() || !networkWorldDescribeItem(item, itemDescriptor)) {
        debug_printf("Multiplayer item drop has an invalid entity.\n");
        return NetworkItemDropDisposition::Reject;
    }
    if (!itemId.has_value()
        && (item->owner != source || item->data.inventory.length != 0)) {
        debug_printf("Multiplayer can only introduce a direct, empty dropped item.\n");
        return NetworkItemDropDisposition::Reject;
    }

    if (launchOptions.mode == NetworkLaunchMode::Host) {
        if (!itemId.has_value()) {
            itemId = networkWorldEnsureItemRegistered(item);
        }
        if (!itemId.has_value()) {
            debug_printf("Multiplayer host could not assign a dropped item entity ID.\n");
            return NetworkItemDropDisposition::Reject;
        }
        bool accepted = submitHostCommand(ItemDropCommand {
            *sourceId,
            *itemId,
            quantity,
            sourceQuantity,
            itemDescriptor,
        });
        return accepted
            ? NetworkItemDropDisposition::DeferToHost
            : NetworkItemDropDisposition::Reject;
    }
    if (launchOptions.mode != NetworkLaunchMode::Join) {
        return NetworkItemDropDisposition::Reject;
    }

    if (!lobby.sendLocalItemDrop(*sourceId,
            itemId.value_or(EntityId {}),
            quantity,
            sourceQuantity,
            networkWorldPhaseRevision(),
            {},
            -1,
            -1,
            itemDescriptor)) {
        debug_printf("Multiplayer item drop command could not be sent.\n");
        return NetworkItemDropDisposition::Reject;
    }
    pendingLocalItemDrop = PendingLocalItemDrop {
        source,
        item,
        *sourceId,
        itemId.value_or(EntityId {}),
        quantity,
    };
    return NetworkItemDropDisposition::DeferToHost;
}

void networkRuntimeHandleLocalItemDrop(Object* source,
    Object* item,
    std::uint32_t quantity,
    std::uint32_t sourceQuantity)
{
    if (!networkWorldActive()
        || networkWorldItemDropInProgress()
        || source == nullptr
        || item == nullptr
        || item->owner != nullptr
        || item->tile < 0) {
        return;
    }
    if (launchOptions.mode != NetworkLaunchMode::Host) {
        return;
    }

    std::optional<EntityId> sourceId = networkWorldFindEntity(source);
    std::optional<EntityId> itemId = networkWorldFindEntity(item);
    ItemDescriptor itemDescriptor;
    if (!sourceId.has_value()
        || !itemId.has_value()
        || !networkWorldDescribeItem(item, itemDescriptor)) {
        debug_printf("Multiplayer item drop could not resolve its authoritative identity.\n");
        return;
    }

    if (!lobby.sendLocalItemDrop(*sourceId,
            *itemId,
            quantity,
            sourceQuantity,
            networkWorldPhaseRevision(),
            networkWorldTakeLastItemSplit(),
            item->tile,
            item->elevation,
            itemDescriptor)) {
        debug_printf("Multiplayer item drop could not be published.\n");
    }
}

bool networkRuntimeHandleLocalMoneyDrop(Object* source, Object* item, std::uint32_t quantity)
{
    if (!networkWorldActive()
        || source == nullptr
        || item == nullptr
        || item->pid != PROTO_ID_MONEY
        || !networkWorldIsLocalItemDrop(source, item)) {
        return false;
    }

    int available = item_count(source, item);
    std::uint32_t sourceQuantity = static_cast<std::uint32_t>(std::max(available, 0));
    NetworkItemDropDisposition disposition = networkRuntimePrepareLocalItemDrop(
        source,
        item,
        quantity,
        sourceQuantity);
    if (disposition == NetworkItemDropDisposition::Reject
        || disposition == NetworkItemDropDisposition::DeferToHost) {
        return true;
    }
    if (!networkWorldApplyLocalItemDrop(source, item, quantity)) {
        debug_printf("Multiplayer host could not apply a local caps drop.\n");
        return true;
    }
    networkRuntimeHandleLocalItemDrop(source, item, quantity, sourceQuantity);
    return true;
}

void networkRuntimeLeaveWorld()
{
    lastSentLocalRotation = -1;
    nextHostCommandSequence = 1;
    pendingRecoveryRequest.reset();
    pendingLocalItemDrop.reset();
    networkWorldLeave();
    agentJournalWriteWorldExit();
}

void networkRuntimeStop()
{
    set_background_processing_when_inactive(false);
    networkWorldLeave();
    agentJournalWriteWorldExit();
    if (backgroundProcessRegistered) {
        remove_bk_process(networkRuntimeBackgroundProcess);
        backgroundProcessRegistered = false;
    }
    lobby.stop();
    bootstrap.stop();
    discardReconnectTransport();
    reconnectTokens.invalidateAll();
    lobbyStarted = false;
    pendingRecoveryRequest.reset();
    pendingLocalItemDrop.reset();
    nextAuthoritativeState = {};
    nextAgentWorldReport = {};
}

} // namespace multiplayer
} // namespace fallout
