#include "multiplayer/network_runtime.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <utility>
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
#include "game/pipboy.h"
#include "game/perk_defs.h"
#include "game/protinst.h"
#include "game/proto.h"
#include "game/proto_types.h"
#include "game/queue.h"
#include "game/roll.h"
#include "game/scripts.h"
#include "game/stat.h"
#include "game/textobj.h"
#include "game/tile.h"
#include "game/worldmap.h"
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
std::optional<PlayerId> announcedWorldMapProposer;
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
    MapTransition,
    ExitGrid,
    Stairs,
    TypedStairsIndependent,
    TypedStairsCrossMap,
    Rest,
    WorldMapTravel,
    CombatTurn,
};
SmokeScenario smokeScenario = SmokeScenario::Movement;
bool smokeWorldMapGuestController = false;
bool smokeWorldMapTakeover = false;
bool smokeWorldMapTown = false;
bool smokeWorldMapEncounter = false;
bool smokeWorldMapQueue = false;
int smokeRestMinutes = 10;
bool smokeRestInterrupt = false;
bool smokeWorldMapState = false;
bool smokeCombatAutoEnd = false;
std::uint64_t smokeCombatAutoEndRevision = 0;
std::vector<PlayerId> smokeCombatObservedOwners;
bool smokeCombatObservedExit = false;
bool smokeCombatReceivedExplorationState = false;

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
    case SmokeScenario::MapTransition:
        return "map-transition";
    case SmokeScenario::ExitGrid:
        return "exit-grid";
    case SmokeScenario::Stairs:
        return "stairs";
    case SmokeScenario::TypedStairsIndependent:
        return "typed-stairs-independent";
    case SmokeScenario::TypedStairsCrossMap:
        return "typed-stairs-cross-map";
    case SmokeScenario::Rest:
        return "rest";
    case SmokeScenario::WorldMapTravel:
        return smokeWorldMapEncounter ? "worldmap-encounter"
            : smokeWorldMapQueue ? "worldmap-queue"
            : smokeWorldMapTown && smokeWorldMapGuestController ? "worldmap-town-guest"
            : smokeWorldMapTown ? "worldmap-town"
            : smokeWorldMapTakeover ? "worldmap-takeover"
            : smokeWorldMapGuestController ? "worldmap-guest" : "worldmap-host";
    case SmokeScenario::CombatTurn:
        return "combat-turn";
    }
    return "unknown";
}

bool isSceneryTransitionSmokeScenario()
{
    return smokeScenario == SmokeScenario::Stairs
        || smokeScenario == SmokeScenario::TypedStairsIndependent
        || smokeScenario == SmokeScenario::TypedStairsCrossMap;
}

bool isAuthorityProbedSmokeScenario()
{
    return smokeScenario == SmokeScenario::Quest
        || isSceneryTransitionSmokeScenario()
        || smokeScenario == SmokeScenario::Rest
        || smokeScenario == SmokeScenario::MapTransition
        || smokeScenario == SmokeScenario::ExitGrid;
}

int lastSentLocalRotation = -1;
std::optional<EventSequence> pendingRecoveryRequest;
std::unique_ptr<Transport> reconnectTransport;
std::chrono::steady_clock::time_point reconnectDeadline;
std::chrono::steady_clock::time_point nextReconnectAttempt;
EventSequence reconnectLastApplied;
std::uint64_t nextHostCommandSequence = 1;
std::uint64_t lastSentCombatEndRevision = 0;
std::chrono::steady_clock::time_point nextAuthoritativeState;
std::chrono::steady_clock::time_point nextAgentWorldReport;
std::optional<std::int32_t> pendingLocalRestRequest;
std::optional<GameCommand> deferredPeerRestCommand;

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
std::optional<EntityId> pendingLocalExitGrid;

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
            bool exitGrid = object != nullptr
                && objectType == OBJ_TYPE_MISC
                && object->pid >= PROTO_ID_0x5000010
                && object->pid <= PROTO_ID_0x5000017;
            if (object == nullptr
                || object->owner != nullptr
                || object->tile < 0
                || (objectType == OBJ_TYPE_MISC && !exitGrid)
                || ((object->flags & OBJECT_HIDDEN) != 0 && !exitGrid)) {
                continue;
            }
            bool door = obj_is_a_portal(object);
            bool container = objectType == OBJ_TYPE_ITEM
                && item_get_type(object) == ITEM_TYPE_CONTAINER;
            int sceneryType = -1;
            Proto* sceneryProto = nullptr;
            bool sceneryTransition = objectType == OBJ_TYPE_SCENERY
                && proto_ptr(object->pid, &sceneryProto) == 0
                && ((sceneryType = sceneryProto->scenery.type) == SCENERY_TYPE_STAIRS
                    || sceneryType == SCENERY_TYPE_LADDER_UP
                    || sceneryType == SCENERY_TYPE_LADDER_DOWN);
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
            state.kind = exitGrid ? "exit" : sceneryTransition
                    ? sceneryType == SCENERY_TYPE_STAIRS ? "stairs" : "ladder"
                : door                                ? "door"
                : container                           ? "container"
                : objectType == OBJ_TYPE_SCENERY      ? "scenery"
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
    appendType(OBJ_TYPE_MISC);
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
    state.pendingRestMinutes = networkWorldPendingRestMinutes();
    state.pendingRestProposerId = networkWorldPendingRestProposer().value;
    state.pendingRestProposerName = networkWorldPendingRestProposerName();
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
    command.expectedPhase = (std::holds_alternative<AttackCommand>(payload)
            || std::holds_alternative<EndTurnCommand>(payload))
        ? SessionPhase::Combat
        : std::holds_alternative<WorldMapRouteCommand>(payload)
        ? SessionPhase::Transition
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
        pendingLocalExitGrid.reset();
        pendingLocalRestRequest.reset();
        deferredPeerRestCommand.reset();
        networkWorldClearRestProposal();
        if (launchOptions.mode == NetworkLaunchMode::Host) {
            networkWorldCancelPendingWorldMapProposal();
            networkWorldHostTakeOverWorldMapTravel();
        }
    }
    if (networkWorldActive()) {
        presentPendingGameChatMessages();
        if (launchOptions.mode == NetworkLaunchMode::Host) {
            networkWorldCombatSetPlayerConnected(kGuestPlayerId,
                lobby.state() == NetworkLobbyState::Ready);
            networkWorldCombatTick();
            if (smokeCombatAutoEnd
                && isInCombat()
                && combat_whose_turn() == obj_dude
                && networkWorldActiveCombatOwner() == kHostPlayerId
                && networkWorldCombatTurnRevision() != smokeCombatAutoEndRevision) {
                smokeCombatAutoEndRevision = networkWorldCombatTurnRevision();
                combat_end_turn();
            }
            if (!networkWorldSynchronizeEnginePhase()) {
                debug_printf("Multiplayer session phase could not follow the engine phase.\n");
            }
            if (!pipboy_is_open() && pendingLocalRestRequest.has_value()) {
                std::int32_t minutes = *pendingLocalRestRequest;
                pendingLocalRestRequest.reset();
                submitHostCommand(RestCommand { minutes });
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
            while (std::optional<GameCommand> command = deferredPeerRestCommand.has_value()
                    ? std::exchange(deferredPeerRestCommand, std::nullopt)
                    : lobby.takePeerCommand()) {
                if (pipboy_is_open() && std::holds_alternative<RestCommand>(command->payload)) {
                    deferredPeerRestCommand = std::move(*command);
                    break;
                }
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
                pendingLocalExitGrid.reset();
                lastSentCombatEndRevision = 0;
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
            } else if (const auto* exitGrid = std::get_if<ExitGridTransitionedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerExitGrid(*exitGrid);
            } else if (const auto* arrival = std::get_if<WorldMapArrivedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerWorldMapArrival(*arrival);
            } else if (const auto* transition = std::get_if<SceneryTransitionedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerSceneryTransition(*transition);
            } else if (const auto* rest = std::get_if<RestStateChangedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerRest(*rest);
            } else if (const auto* modal = std::get_if<SharedModalStateChangedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerSharedModal(*modal);
            } else if (const auto* route = std::get_if<WorldMapRouteSelectedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerWorldMapRoute(*route);
            } else if (const auto* transfer = std::get_if<InventoryTransferredEvent>(&event->payload)) {
                applied = networkWorldApplyInventoryTransfer(*transfer);
            } else if (const auto* drop = std::get_if<ItemDroppedEvent>(&event->payload)) {
                applied = networkWorldApplyItemDrop(*drop);
            } else if (const auto* attack = std::get_if<AttackStartedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerAttack(*attack);
            } else if (const auto* combat = std::get_if<CombatTurnStateChangedEvent>(&event->payload)) {
                applied = networkWorldApplyPeerCombatTurn(*combat);
                if (applied) {
                    if (smokeScenario == SmokeScenario::CombatTurn) {
                        if (combat->phase == SessionPhase::Exploration) {
                            smokeCombatObservedExit = true;
                        } else if (!combat->state.initiative.empty()) {
                            PlayerId owner = combat->state.initiative[combat->state.activeIndex].ownerId;
                            smokeCombatObservedOwners.push_back(owner);
                        }
                    }
                    lastSentCombatEndRevision = 0;
                    if (combat->phase == SessionPhase::Exploration) {
                        char message[] = "Combat ended.";
                        display_print(message);
                    } else if (networkWorldActiveCombatOwner() == kGuestPlayerId) {
                        char message[] = "Your combat turn. Press Space to end turn.";
                        display_print(message);
                    } else if (networkWorldActiveCombatOwner() == kHostPlayerId) {
                        char message[] = "Host player's combat turn.";
                        display_print(message);
                    } else {
                        char message[] = "Host AI combat turn.";
                        display_print(message);
                    }
                }
            }
            if (!applied) {
                debug_printf("Multiplayer peer event could not be applied.\n");
            } else {
                if (std::holds_alternative<ExitGridTransitionedEvent>(event->payload)) {
                    pendingLocalExitGrid.reset();
                }
                if (const auto* drop = std::get_if<ItemDroppedEvent>(&event->payload)) {
                    continuePendingLocalItemDrop(*drop);
                }
                if (!lobby.confirmPeerEventApplied(event->sequence)) {
                    debug_printf("Multiplayer peer event boundary could not be confirmed.\n");
                }
            }
        }
        while (std::optional<WorldSnapshot> state = lobby.takeAuthoritativeState()) {
            // The lobby drains ordered events before state packets. A state
            // captured before a map-arrival event must not roll the freshly
            // loaded destination back to its old transition phase.
            if (state->lastIncludedEvent.value < lobby.lastAppliedEvent().value) {
                continue;
            }
            if (!networkWorldApplyAuthoritativeState(*state)) {
                debug_printf("Multiplayer authoritative state could not be applied.\n");
            } else if (smokeScenario == SmokeScenario::CombatTurn
                && state->phase == SessionPhase::Exploration
                && smokeCombatObservedExit) {
                smokeCombatReceivedExplorationState = true;
            }
        }
        if (pendingLocalExitGrid.has_value()
            && networkWorldPhase() == SessionPhase::Exploration
            && networkWorldReadyLocalExitGrid() != pendingLocalExitGrid) {
            pendingLocalExitGrid.reset();
        }
        if (!pendingLocalExitGrid.has_value()) {
            std::optional<EntityId> readyExit = networkWorldReadyLocalExitGrid();
            if (readyExit.has_value()) {
                networkRuntimeSubmitLocalExitGrid(*readyExit);
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
    smokeRestMinutes = 10;
    smokeRestInterrupt = false;
    smokeWorldMapState = false;
    smokeWorldMapGuestController = false;
    smokeWorldMapTakeover = false;
    smokeWorldMapTown = false;
    smokeWorldMapEncounter = false;
    smokeWorldMapQueue = false;
    announcedWorldMapProposer.reset();
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
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=map-transition") == 0) {
            smokeScenario = SmokeScenario::MapTransition;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=exit-grid") == 0) {
            smokeScenario = SmokeScenario::ExitGrid;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=stairs") == 0) {
            smokeScenario = SmokeScenario::Stairs;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=typed-stairs-independent") == 0) {
            smokeScenario = SmokeScenario::TypedStairsIndependent;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=typed-stairs-cross-map") == 0) {
            smokeScenario = SmokeScenario::TypedStairsCrossMap;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=rest") == 0) {
            smokeScenario = SmokeScenario::Rest;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=combat-turn") == 0) {
            smokeScenario = SmokeScenario::CombatTurn;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=worldmap-host") == 0) {
            smokeScenario = SmokeScenario::WorldMapTravel;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=worldmap-guest") == 0) {
            smokeScenario = SmokeScenario::WorldMapTravel;
            smokeWorldMapGuestController = true;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=worldmap-takeover") == 0) {
            smokeScenario = SmokeScenario::WorldMapTravel;
            smokeWorldMapGuestController = true;
            smokeWorldMapTakeover = true;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=worldmap-town") == 0) {
            smokeScenario = SmokeScenario::WorldMapTravel;
            smokeWorldMapTown = true;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=worldmap-town-guest") == 0) {
            smokeScenario = SmokeScenario::WorldMapTravel;
            smokeWorldMapTown = true;
            smokeWorldMapGuestController = true;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=worldmap-encounter") == 0) {
            smokeScenario = SmokeScenario::WorldMapTravel;
            smokeWorldMapEncounter = true;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-scenario=worldmap-queue") == 0) {
            smokeScenario = SmokeScenario::WorldMapTravel;
            smokeWorldMapQueue = true;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-rest-interrupt") == 0) {
            smokeRestInterrupt = true;
        } else if (argv[index] != nullptr && std::strcmp(argv[index], "--multiplayer-smoke-worldmap-state") == 0) {
            smokeWorldMapState = true;
        } else if (argv[index] != nullptr
            && std::strncmp(argv[index], "--multiplayer-smoke-rest-minutes=", 33) == 0) {
            char* end = nullptr;
            long minutes = std::strtol(argv[index] + 33, &end, 10);
            if (end == argv[index] + 33 || *end != '\0'
                || minutes < 0 || minutes > 360
                || !isValidRestMinutes(static_cast<int>(minutes))
                || minutes == 0) {
                std::fprintf(stderr, "Invalid --multiplayer-smoke-rest-minutes value.\n");
                return false;
            }
            smokeRestMinutes = static_cast<int>(minutes);
        } else if (argv[index] != nullptr
            && std::strncmp(argv[index], "--multiplayer-smoke-rest-choice=", 32) == 0) {
            const char* choice = argv[index] + 32;
            smokeRestMinutes = std::strcmp(choice, "until_morning") == 0 ? kRestUntilMorning
                : std::strcmp(choice, "until_noon") == 0 ? kRestUntilNoon
                : std::strcmp(choice, "until_evening") == 0 ? kRestUntilEvening
                : std::strcmp(choice, "until_midnight") == 0 ? kRestUntilMidnight
                : std::strcmp(choice, "until_healed") == 0 ? kRestUntilHealed
                : 0;
            if (smokeRestMinutes == 0) {
                std::fprintf(stderr, "Invalid --multiplayer-smoke-rest-choice value.\n");
                return false;
            }
        }
    }
    if (smokeTestEnabled && launchOptions.mode == NetworkLaunchMode::Disabled) {
        std::fprintf(stderr, "--multiplayer-smoke-test requires --multiplayer-host or --multiplayer-join.\n");
        return false;
    }
    if (smokeRestInterrupt && (smokeScenario != SmokeScenario::Rest
            || smokeRestMinutes != kRestUntilMorning)) {
        std::fprintf(stderr, "The rest interruption fixture requires rest until_morning.\n");
        return false;
    }
    if (smokeWorldMapState && smokeScenario != SmokeScenario::Movement) {
        std::fprintf(stderr, "The world-map state fixture requires the movement scenario.\n");
        return false;
    }
    pendingLocalSheet.reset();
    smokeCombatAutoEnd = false;
    smokeCombatAutoEndRevision = 0;
    smokeCombatObservedOwners.clear();
    smokeCombatObservedExit = false;
    smokeCombatReceivedExplorationState = false;
    pendingRecoveryRequest.reset();
    pendingLocalItemDrop.reset();
    pendingLocalExitGrid.reset();
    pendingLocalRestRequest.reset();
    deferredPeerRestCommand.reset();
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
    if (smokeScenario == SmokeScenario::TypedStairsIndependent
        || smokeScenario == SmokeScenario::TypedStairsCrossMap) {
        return "ChilDrn2.map";
    }
    if (smokeScenario == SmokeScenario::Quest || smokeScenario == SmokeScenario::Rest) {
        return "ShadyW.map";
    }
    if (smokeScenario == SmokeScenario::Elevation) {
        return "Vault13.map";
    }
    if (smokeScenario == SmokeScenario::MapTransition) {
        return "Brohd12.map";
    }
    if (smokeScenario == SmokeScenario::Stairs) {
        return "WatrShd.map";
    }
    return smokeScenario == SmokeScenario::ExitGrid ? "Vault13.map" : "V13Ent.map";
}

static bool runLiveWorldMapTravelSmoke(const SessionId sessionId)
{
    const int kStartX = smokeWorldMapQueue ? 1200 : smokeWorldMapTown ? 1072 : 1325;
    const int kStartY = smokeWorldMapQueue ? 1200 : smokeWorldMapTown ? 75 : 325;
    const int kFirstTargetX = kStartX + 3;
    const int kFinalTargetX = kStartX + 4;
    const int expectedMap = smokeWorldMapTown ? MAP_SHADYW
        : smokeWorldMapEncounter || smokeWorldMapQueue ? -1 : MAP_CITY1;
    PlayerId proposer = smokeWorldMapGuestController ? kGuestPlayerId : kHostPlayerId;
    PlayerId local = launchOptions.mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    WorldMapState fixture;
    worldmap_capture_state(fixture);
    fixture.x = kStartX;
    fixture.y = kStartY;
    if (!worldmap_apply_state(fixture)) {
        setStatus("MULTIPLAYER WORLD-MAP SMOKE FAILED: START POSITION");
        return false;
    }
    if (launchOptions.mode == NetworkLaunchMode::Host) {
        game_global_vars[GVAR_VAULT_WATER] = 1;
        game_global_vars[GVAR_VATS_COUNTDOWN] = 0;
        game_global_vars[GVAR_COUNTDOWN_TO_DESTRUCTION] = 0;
    }

    bool proposed = false;
    bool accepted = false;
    bool firstRoute = false;
    bool changedRoute = false;
    bool landed = false;
    bool hostMoved = false;
    bool reconnectRequested = false;
    bool sawDisconnect = false;
    bool reconnectObserved = false;
    bool specialPrepared = false;
    bool specialTriggered = false;
    EventSequence disconnectedAt;
    auto arrivedAt = std::chrono::steady_clock::time_point {};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
    engineExecutionProbeBegin();
    while (std::chrono::steady_clock::now() < deadline) {
        networkRuntimeBackgroundProcess();
        sawDisconnect = sawDisconnect || lobby.state() == NetworkLobbyState::Disconnected;
        reconnectObserved = reconnectObserved
            || (sawDisconnect && lobby.state() == NetworkLobbyState::Ready);
        WorldMapState position;
        worldmap_capture_state(position);

        if (launchOptions.mode == NetworkLaunchMode::Join
            && (!smokeWorldMapGuestController || smokeWorldMapTakeover)
            && !reconnectRequested
            && networkWorldWorldMapTravelApproved()
            && ((smokeWorldMapEncounter || smokeWorldMapQueue)
                ? networkWorldSelectedWorldMapRoute().has_value()
                : position.x >= kFinalTargetX
                    && networkWorldSelectedWorldMapRoute() == std::make_pair(kFinalTargetX, kStartY))) {
            disconnectedAt = lobby.lastAppliedEvent();
            reconnectRequested = lobby.disconnectForReconnect();
            if (reconnectRequested) {
                nextReconnectAttempt = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            }
        }

        if (local == proposer && !proposed && networkWorldPhase() == SessionPhase::Exploration) {
            proposed = networkRuntimeRequestSharedModal(SharedModalKind::WorldMap, true);
        }
        if (local != proposer && !accepted
            && networkWorldPendingWorldMapProposer() == proposer) {
            accepted = networkRuntimeRequestSharedModal(SharedModalKind::WorldMap, true);
        }
        if (networkWorldWorldMapTravelApproved()) {
            if (local == proposer) {
                if (!firstRoute) {
                    firstRoute = networkRuntimeSubmitLocalWorldMapRoute(kFirstTargetX, kStartY);
                } else if (!changedRoute && position.x > kStartX
                    && !smokeWorldMapEncounter && !smokeWorldMapQueue) {
                    changedRoute = networkRuntimeSubmitLocalWorldMapRoute(kFinalTargetX, kStartY);
                } else if (!smokeWorldMapTakeover && !smokeWorldMapTown
                    && !smokeWorldMapEncounter && !smokeWorldMapQueue
                    && changedRoute && !landed && position.x >= kFinalTargetX
                    && (smokeWorldMapGuestController
                        || lobby.state() == NetworkLobbyState::Disconnected)) {
                    landed = networkRuntimeRequestSharedModal(SharedModalKind::WorldMap, false);
                }
            }
            if (smokeWorldMapTakeover
                && launchOptions.mode == NetworkLaunchMode::Host
                && !landed
                && lobby.state() == NetworkLobbyState::Disconnected
                && networkWorldLocalWorldMapController()
                && networkWorldWorldMapDeparted()
                && position.x >= kFinalTargetX) {
                landed = networkRuntimeRequestSharedModal(SharedModalKind::WorldMap, false);
            }
            if (smokeWorldMapTown
                && launchOptions.mode == NetworkLaunchMode::Host
                && !landed
                && position.x >= kFinalTargetX
                && networkWorldSelectedWorldMapRoute() == std::make_pair(kFinalTargetX, kStartY)
                && (smokeWorldMapGuestController
                    || lobby.state() == NetworkLobbyState::Disconnected)) {
                landed = networkWorldFinishWorldMapTravel(WorldMapArrivalKind::City);
            }
            if (launchOptions.mode == NetworkLaunchMode::Host
                && networkWorldSelectedWorldMapRoute().has_value()) {
                if ((smokeWorldMapEncounter || smokeWorldMapQueue)
                    && lobby.state() != NetworkLobbyState::Disconnected) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                if ((smokeWorldMapEncounter || smokeWorldMapQueue) && !specialPrepared) {
                    if (smokeWorldMapEncounter) {
                        auto route = networkWorldSelectedWorldMapRoute();
                        if (!route.has_value()
                            || !worldmap_authoritative_travel_begin(route->first, route->second)) {
                            break;
                        }
                        WorldMapTravelProgress progress;
                        worldmap_capture_travel_progress(progress);
                        progress.miles = progress.dayLength - 1;
                        progress.timeAdder = 0;
                        if (!worldmap_apply_travel_progress(progress)) break;
                        int encounterSeed = 0;
                        for (int seed = 1; seed <= 1024; seed++) {
                            roll_set_seed(seed);
                            int chance = roll_random(1, 6) + roll_random(1, 6) + roll_random(1, 6);
                            if (chance < 9) { encounterSeed = seed; break; }
                        }
                        if (encounterSeed == 0) break;
                        roll_set_seed(encounterSeed);
                    }
                    if (smokeWorldMapQueue) {
                        auto* withdrawal = static_cast<WithdrawalEvent*>(mem_malloc(sizeof(WithdrawalEvent)));
                        if (withdrawal == nullptr) break;
                        *withdrawal = WithdrawalEvent { 0, 0, PERK_BUFFOUT_ADDICTION };
                        if (queue_add(1, obj_dude, withdrawal, EVENT_TYPE_WITHDRAWAL) == -1) {
                            mem_free(withdrawal);
                            break;
                        }
                    }
                    specialPrepared = true;
                }
                WorldMapTravelStepResult step = networkWorldAdvanceWorldMapTravel();
                hostMoved = hostMoved || step.x > kStartX;
                if ((smokeWorldMapEncounter && step.status == WorldMapTravelStepStatus::Encounter)
                    || (smokeWorldMapQueue && step.status == WorldMapTravelStepStatus::QueueInterrupted)) {
                    specialTriggered = true;
                    landed = networkWorldFinishWorldMapTravel(
                        smokeWorldMapEncounter ? WorldMapArrivalKind::Encounter
                                               : WorldMapArrivalKind::Interrupted,
                        step.specialEncounter);
                    if (!landed) break;
                    continue;
                }
                if (step.status != WorldMapTravelStepStatus::Moving
                    && step.status != WorldMapTravelStepStatus::Arrived) {
                    std::fprintf(stderr, "World-map smoke step stopped with status=%d at %d,%d.\n",
                        static_cast<int>(step.status), step.x, step.y);
                    break;
                }
            }
        }

        if (networkWorldPhase() == SessionPhase::Exploration
            && (expectedMap < 0
                ? map_data.field_34 != MAP_V13ENT && map_data.field_34 >= 0
                : map_data.field_34 == expectedMap)
            && (smokeWorldMapEncounter || smokeWorldMapQueue
                ? position.x > kStartX && position.y == kStartY
                : position.x == kFinalTargetX && position.y == kStartY)) {
            if (launchOptions.mode == NetworkLaunchMode::Join
                && ((smokeWorldMapGuestController && !smokeWorldMapTakeover)
                    || smokeWorldMapEncounter || smokeWorldMapQueue)
                && !reconnectRequested) {
                disconnectedAt = lobby.lastAppliedEvent();
                reconnectRequested = lobby.disconnectForReconnect();
                if (reconnectRequested) {
                    nextReconnectAttempt = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                }
            }
            if (arrivedAt.time_since_epoch().count() == 0) {
                arrivedAt = std::chrono::steady_clock::now();
            }
            if (reconnectObserved
                && (launchOptions.mode == NetworkLaunchMode::Host
                    || lobby.state() == NetworkLobbyState::Ready)
                && std::chrono::steady_clock::now() - arrivedAt >= std::chrono::milliseconds(
                    launchOptions.mode == NetworkLaunchMode::Host ? 2000 : 700)) {
                break;
            }
        }
        if (networkRuntimeFailed()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EngineExecutionProbeCounts counts = engineExecutionProbeEnd();
    WorldSnapshot snapshot;
    SnapshotDigestResult digest;
    WorldMapState finalPosition;
    worldmap_capture_state(finalPosition);
    bool passed = networkWorldPhase() == SessionPhase::Exploration
        && (expectedMap < 0
            ? map_data.field_34 != MAP_V13ENT && map_data.field_34 >= 0
            : map_data.field_34 == expectedMap)
        && (smokeWorldMapEncounter || smokeWorldMapQueue
            ? finalPosition.x > kStartX && finalPosition.y == kStartY
            : finalPosition.x == kFinalTargetX && finalPosition.y == kStartY)
        && arrivedAt.time_since_epoch().count() != 0
        && reconnectObserved
        && (launchOptions.mode == NetworkLaunchMode::Host
            || lobby.state() == NetworkLobbyState::Ready)
        && (local != proposer
            || (proposed && firstRoute
                && ((smokeWorldMapEncounter || smokeWorldMapQueue) ? true : changedRoute)
                && ((smokeWorldMapEncounter || smokeWorldMapQueue)
                    ? (launchOptions.mode == NetworkLaunchMode::Host && specialTriggered && landed)
                    : smokeWorldMapTakeover ? reconnectRequested
                    : smokeWorldMapTown && launchOptions.mode == NetworkLaunchMode::Join
                        ? reconnectRequested
                        : landed)))
        && (local == proposer || accepted)
        && (!smokeWorldMapTakeover || launchOptions.mode != NetworkLaunchMode::Host || landed)
        && (launchOptions.mode != NetworkLaunchMode::Join
            || !reconnectRequested
            || !(!smokeWorldMapGuestController || smokeWorldMapTakeover)
            || lobby.lastAppliedEvent().value > disconnectedAt.value)
        && (launchOptions.mode != NetworkLaunchMode::Host || hostMoved)
        && (launchOptions.mode != NetworkLaunchMode::Join
            || (counts.scriptProcedures == 0 && counts.combatAttacks == 0 && counts.randomDraws == 0))
        && networkWorldCaptureAuthoritativeState(
            launchOptions.mode == NetworkLaunchMode::Host
                ? lobby.latestAuthoritativeEvent()
                : lobby.lastAppliedEvent(), snapshot)
        && (digest = computeSnapshotDigest(snapshot));
    if (!passed) {
        std::fprintf(stderr,
            "World-map smoke state role=%s proposed=%d accepted=%d route=%d changed=%d landed=%d moved=%d phase=%d map=%d pos=%d,%d rules=%u/%u/%u.\n",
            launchOptions.mode == NetworkLaunchMode::Host ? "host" : "guest",
            proposed, accepted, firstRoute, changedRoute, landed, hostMoved,
            static_cast<int>(networkWorldPhase()), map_data.field_34,
            finalPosition.x, finalPosition.y,
            counts.scriptProcedures, counts.combatAttacks, counts.randomDraws);
        setStatus("MULTIPLAYER WORLD-MAP SMOKE FAILED: LIVE TRAVEL");
        return false;
    }
    std::fprintf(stdout,
        "MULTIPLAYER_SMOKE_TEST_PASS role=%s session=%llu command=%s map=%d world=%d,%d digest=%llu scripts=%u attacks=%u rng=%u\n",
        launchOptions.mode == NetworkLaunchMode::Host ? "host" : "guest",
        static_cast<unsigned long long>(sessionId.value),
        smokeScenarioName(), map_data.field_34,
        finalPosition.x, finalPosition.y,
        static_cast<unsigned long long>(digest.digest.overall),
        counts.scriptProcedures, counts.combatAttacks, counts.randomDraws);
    std::fprintf(stdout,
        "WORLD_MAP_DIGEST role=%s event=%llu phase=%u clock=%d session=%llu actors=%llu critters=%llu doors=%llu scenery=%llu items=%llu globals=%llu mapvars=%llu queue=%llu worldmap=%llu\n",
        launchOptions.mode == NetworkLaunchMode::Host ? "host" : "guest",
        static_cast<unsigned long long>(snapshot.lastIncludedEvent.value),
        snapshot.phaseRevision, snapshot.gameTime,
        static_cast<unsigned long long>(digest.digest.session),
        static_cast<unsigned long long>(digest.digest.actors),
        static_cast<unsigned long long>(digest.digest.critters),
        static_cast<unsigned long long>(digest.digest.doors),
        static_cast<unsigned long long>(digest.digest.scenery),
        static_cast<unsigned long long>(digest.digest.items),
        static_cast<unsigned long long>(digest.digest.globals),
        static_cast<unsigned long long>(digest.digest.mapVariables),
        static_cast<unsigned long long>(digest.digest.timedEvents),
        static_cast<unsigned long long>(digest.digest.worldMap));
    std::fflush(stdout);
    return true;
}

static bool runLiveCombatTurnSmoke(SessionId sessionId)
{
    const bool host = launchOptions.mode == NetworkLaunchMode::Host;
    const std::uint32_t explorationRevision = networkWorldPhaseRevision();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    engineExecutionProbeBegin();
    if (host) {
        smokeCombatAutoEnd = true;
        // No hostile action is needed: this exercises the real sequential
        // engine loop with two owned actors and semantic end-turn over TLS.
        combat(nullptr);
        smokeCombatAutoEnd = false;
    }
    while (std::chrono::steady_clock::now() < deadline) {
        networkRuntimeBackgroundProcess();
        if (!host && networkWorldActiveCombatOwner() == kGuestPlayerId) {
            networkRuntimeHandleCombatInput(KEY_SPACE);
        }
        if (networkWorldPhase() == SessionPhase::Exploration
            && networkWorldPhaseRevision() >= explorationRevision + 2
            && (host || (smokeCombatObservedExit
                    && smokeCombatReceivedExplorationState))) {
            if (!host) {
                break;
            }
            // Give the post-combat checkpoint time to reach the replica.
            auto settleUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
            while (std::chrono::steady_clock::now() < settleUntil) {
                networkRuntimeBackgroundProcess();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EngineExecutionProbeCounts counts = engineExecutionProbeEnd();
    WorldSnapshot snapshot;
    SnapshotDigestResult digest;
    std::string observedOwners;
    for (PlayerId owner : smokeCombatObservedOwners) {
        if (!observedOwners.empty()) {
            observedOwners += ',';
        }
        observedOwners += std::to_string(owner.value);
    }
    bool sawOrderedTurns = host
        ? smokeCombatAutoEndRevision != 0
        : smokeCombatObservedOwners.size() >= 2
            && smokeCombatObservedOwners[0] == kHostPlayerId
            && smokeCombatObservedOwners[1] == kGuestPlayerId;
    bool passed = sawOrderedTurns
        && networkWorldPhase() == SessionPhase::Exploration
        && networkWorldPhaseRevision() >= explorationRevision + 2
        && (host || (smokeCombatObservedExit
                && smokeCombatReceivedExplorationState
                && counts.scriptProcedures == 0
                && counts.combatAttacks == 0
                && counts.randomDraws == 0))
        && networkWorldCaptureAuthoritativeState(
            host ? lobby.latestAuthoritativeEvent() : lobby.lastAppliedEvent(), snapshot)
        && (digest = computeSnapshotDigest(snapshot));
    if (!passed) {
        std::fprintf(stderr,
            "Combat smoke failed role=%s phase=%d revision=%u turns=%zu end=%d state=%d auto=%llu rules=%u/%u/%u.\n",
            host ? "host" : "guest", static_cast<int>(networkWorldPhase()),
            networkWorldPhaseRevision(), smokeCombatObservedOwners.size(),
            smokeCombatObservedExit, smokeCombatReceivedExplorationState,
            static_cast<unsigned long long>(smokeCombatAutoEndRevision),
            counts.scriptProcedures, counts.combatAttacks, counts.randomDraws);
        setStatus("MULTIPLAYER COMBAT SMOKE FAILED: TURN OWNERSHIP");
        return false;
    }
    std::fprintf(stdout,
        "MULTIPLAYER_SMOKE_TEST_PASS role=%s session=%llu command=combat-turn phase=%u event=%llu owners=%s digest=%llu scripts=%u attacks=%u rng=%u\n",
        host ? "host" : "guest",
        static_cast<unsigned long long>(sessionId.value),
        snapshot.phaseRevision,
        static_cast<unsigned long long>(snapshot.lastIncludedEvent.value),
        host ? "host-authoritative" : observedOwners.c_str(),
        static_cast<unsigned long long>(digest.digest.overall),
        counts.scriptProcedures, counts.combatAttacks, counts.randomDraws);
    std::fflush(stdout);
    return true;
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
    auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(smokeRestInterrupt ? 60 : 25);
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
            if ((smokeScenario == SmokeScenario::WorldMapTravel
                    || smokeScenario == SmokeScenario::CombatTurn)
                && !lobby.startRequested()) {
                if (launchOptions.mode == NetworkLaunchMode::Host
                    && !networkRuntimeRequestStart()) {
                    setStatus("MULTIPLAYER WORLD-MAP SMOKE FAILED: LOBBY START");
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
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
            // Do not interpret the actors' spawn hex as a new exit-grid step.
            pendingLocalExitGrid = networkWorldReadyLocalExitGrid();
            if (!networkWorldRunPartyExperienceSmokeTest()) {
                setStatus("MULTIPLAYER SMOKE TEST FAILED: PARTY EXPERIENCE AUTHORITY");
                break;
            }

            if (smokeScenario == SmokeScenario::WorldMapTravel) {
                if (runLiveWorldMapTravelSmoke(bootstrap.sessionId())) return true;
                break;
            }
            if (smokeScenario == SmokeScenario::CombatTurn) {
                if (runLiveCombatTurnSmoke(bootstrap.sessionId())) return true;
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
            std::optional<MapTransitionSmokeFixture> mapTransitionFixture;
            std::optional<ExitGridSmokeFixture> exitGridFixture;
            std::optional<SceneryTransitionSmokeFixture> sceneryTransitionFixture;
            int restStartingGameTime = game_time();
            WorldMapState initialWorldMap;
            worldmap_capture_state(initialWorldMap);
            auto worldMapStateMatchesFixture = [&](const WorldMapState& state) {
                return !smokeWorldMapState
                    || (state.specialEncounters == (initialWorldMap.specialEncounters ^ 1)
                        && state.grid[42] == (initialWorldMap.grid[42] == 0 ? 1 : 0)
                        && state.knownTownEntrances[4]
                            == (initialWorldMap.knownTownEntrances[4] == 0 ? 1 : 0)
                        && state.x == 1327
                        && state.y == 325);
            };
            if (smokeScenario == SmokeScenario::Rest && smokeRestInterrupt
                && launchOptions.mode == NetworkLaunchMode::Host) {
                // Fallout's withdrawal handler returns nonzero for the story
                // actor. Ending a nonexistent addiction is otherwise inert.
                auto* withdrawal = static_cast<WithdrawalEvent*>(mem_malloc(sizeof(WithdrawalEvent)));
                if (withdrawal == nullptr) {
                    setStatus("MULTIPLAYER SMOKE TEST FAILED: REST INTERRUPT FIXTURE");
                    break;
                }
                *withdrawal = WithdrawalEvent { 0, 0, PERK_BUFFOUT_ADDICTION };
                if (queue_add(600, networkWorldPlayerActor(kHostPlayerId), withdrawal,
                        EVENT_TYPE_WITHDRAWAL) == -1) {
                    mem_free(withdrawal);
                    setStatus("MULTIPLAYER SMOKE TEST FAILED: REST INTERRUPT QUEUE");
                    break;
                }
            }
            int restExpectedMinutes = smokeRestMinutes > 0 ? smokeRestMinutes
                : smokeRestMinutes == kRestUntilHealed ? 180
                : restMinutesUntilHour(smokeRestMinutes, game_time_hour());
            int restCompletionGameTime = 0;
            bool restCompletionInterrupted = false;
            auto restTimeReached = [&](int finalTime) {
                return smokeRestInterrupt
                    ? finalTime > restStartingGameTime
                        && finalTime < restStartingGameTime + restExpectedMinutes * 600
                    : finalTime >= restStartingGameTime + restExpectedMinutes * 600;
            };
            int restHostInitialHits = 0;
            int restGuestInitialHits = 0;
            if (smokeScenario == SmokeScenario::Rest
                && (restExpectedMinutes >= 180 || smokeRestMinutes == kRestUntilHealed)) {
                Object* restHost = networkWorldPlayerActor(kHostPlayerId);
                Object* restGuest = networkWorldPlayerActor(kGuestPlayerId);
                if (restHost == nullptr || restGuest == nullptr
                    || critter_get_hits(restHost) <= 3
                    || critter_get_hits(restGuest) <= 3) {
                    setStatus("MULTIPLAYER SMOKE TEST FAILED: REST HEALING FIXTURE");
                    break;
                }
                critter_adjust_hits(restHost, -3);
                critter_adjust_hits(restGuest, -3);
                restHostInitialHits = critter_get_hits(restHost);
                restGuestInitialHits = critter_get_hits(restGuest);
            }
            auto restHealed = [&]() {
                if (restExpectedMinutes < 180 && smokeRestMinutes != kRestUntilHealed) {
                    return true;
                }
                Object* restHost = networkWorldPlayerActor(kHostPlayerId);
                Object* restGuest = networkWorldPlayerActor(kGuestPlayerId);
                if (smokeRestInterrupt) {
                    return restHost != nullptr && restGuest != nullptr
                        && critter_get_hits(restHost) == restHostInitialHits
                        && critter_get_hits(restGuest) == restGuestInitialHits;
                }
                return restHost != nullptr
                    && restGuest != nullptr
                    && critter_get_hits(restHost) > restHostInitialHits
                    && critter_get_hits(restGuest) > restGuestInitialHits
                    && (smokeRestMinutes != kRestUntilHealed
                        || (critter_get_hits(restHost) >= stat_level(restHost, STAT_MAXIMUM_HIT_POINTS)
                            && critter_get_hits(restGuest) >= stat_level(restGuest, STAT_MAXIMUM_HIT_POINTS)));
            };
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
            } else if (smokeScenario == SmokeScenario::MapTransition) {
                mapTransitionFixture = networkWorldPrepareMapTransitionSmokeTest();
                scenarioStartingTile = scenarioActor != nullptr ? scenarioActor->tile : -1;
            } else if (smokeScenario == SmokeScenario::ExitGrid) {
                exitGridFixture = networkWorldPrepareExitGridSmokeTest();
                if (exitGridFixture.has_value()) {
                    scenarioTargetId = exitGridFixture->exitId;
                }
                scenarioStartingTile = scenarioActor != nullptr ? scenarioActor->tile : -1;
            } else if (isSceneryTransitionSmokeScenario()) {
                sceneryTransitionFixture = smokeScenario == SmokeScenario::Stairs
                    ? networkWorldPrepareSceneryTransitionSmokeTest()
                    : networkWorldPrepareSceneryTransitionSmokeTest(
                        true,
                        smokeScenario == SmokeScenario::TypedStairsIndependent ? 18900 : 18894,
                        smokeScenario == SmokeScenario::TypedStairsCrossMap);
                if (sceneryTransitionFixture.has_value()) {
                    scenarioTargetId = sceneryTransitionFixture->transitionId;
                }
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
                    && smokeScenario != SmokeScenario::MapTransition
                    && smokeScenario != SmokeScenario::ExitGrid
                    && !isSceneryTransitionSmokeScenario()
                    && smokeScenario != SmokeScenario::Rest
                    && !scenarioTargetId.has_value())
                || (smokeScenario == SmokeScenario::Elevation && !elevatorFixture.has_value())
                || (smokeScenario == SmokeScenario::MapTransition && !mapTransitionFixture.has_value())
                || (smokeScenario == SmokeScenario::ExitGrid && !exitGridFixture.has_value())
                || (isSceneryTransitionSmokeScenario() && !sceneryTransitionFixture.has_value())
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
            case SmokeScenario::MapTransition:
                scenarioCommand.payload = ElevatorCommand { mapTransitionFixture->elevatorType, mapTransitionFixture->destinationLevel };
                break;
            case SmokeScenario::ExitGrid:
                scenarioCommand.payload = ExitGridCommand { exitGridFixture->exitId };
                break;
            case SmokeScenario::Stairs:
            case SmokeScenario::TypedStairsIndependent:
            case SmokeScenario::TypedStairsCrossMap:
                scenarioCommand.payload = SceneryTransitionCommand { sceneryTransitionFixture->transitionId };
                break;
            case SmokeScenario::Rest:
                scenarioCommand.payload = RestCommand { smokeRestMinutes };
                break;
            }
            if (!scenarioCommandReady) {
                break;
            }
            EventSequence scenarioFinalEventSequence {
                smokeScenario == SmokeScenario::Pickup || smokeScenario == SmokeScenario::Rest ? 2ULL : 1ULL
            };

            bool gameplayPassed = false;
            std::vector<GameEvent> authoritativeEvents;
            if (isAuthorityProbedSmokeScenario()) {
                engineExecutionProbeBegin();
            }
            if (smokeScenario == SmokeScenario::Rest && launchOptions.mode == NetworkLaunchMode::Host) {
                GameCommand hostProposal;
                hostProposal.sequence.value = 1;
                hostProposal.playerId = kHostPlayerId;
                hostProposal.actorId = EntityId { kHostPlayerId.value };
                hostProposal.expectedPhase = SessionPhase::Exploration;
                hostProposal.expectedPhaseRevision = networkWorldPhaseRevision();
                hostProposal.payload = RestCommand { smokeRestMinutes };
                AuthoritativeCommandResult proposal = networkWorldProcessCommand(hostProposal);
                const auto* proposed = proposal.event.has_value()
                    ? std::get_if<RestStateChangedEvent>(&proposal.event->payload)
                    : nullptr;
                if (proposal.result.status != CommandStatus::Accepted
                    || proposed == nullptr
                    || proposed->completed
                    || game_time() != restStartingGameTime
                    || networkWorldPendingRestMinutes() != smokeRestMinutes) {
                    setStatus("MULTIPLAYER SMOKE TEST FAILED: REST PROPOSAL ADVANCED TIME");
                    break;
                }
                authoritativeEvents.push_back(*proposal.event);
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
                                && result.result.firstEventSequence == EventSequence { smokeScenario == SmokeScenario::Rest ? 2ULL : 1ULL }
                                && result.result.eventCount == 1;
                        } else if (decoded.envelope.kind == MessageKind::Event) {
                            GameEventDecodeResult event = decodeGameEvent(decoded.envelope);
                            bool eventApplied = false;
                            EventSequence expectedEventSequence { receivedEventCount + 1 };
                            if (event && smokeScenario == SmokeScenario::Rest
                                && event.event.sequence == expectedEventSequence) {
                                const auto* rest = std::get_if<RestStateChangedEvent>(&event.event.payload);
                                if (receivedEventCount == 0) {
                                    eventApplied = rest != nullptr
                                        && event.event.causedBy == CommandSequence { 1 }
                                        && rest->actorId == EntityId { kHostPlayerId.value }
                                        && rest->minutes == smokeRestMinutes
                                        && !rest->completed
                                        && networkWorldApplyPeerRest(*rest)
                                        && networkWorldPendingRestMinutes() == smokeRestMinutes
                                        && game_time() == restStartingGameTime;
                                } else {
                                    eventApplied = rest != nullptr
                                        && event.event.causedBy == scenarioCommand.sequence
                                        && rest->actorId == scenarioCommand.actorId
                                        && rest->minutes == smokeRestMinutes
                                        && rest->completed
                                        && rest->interrupted == smokeRestInterrupt
                                        && networkWorldApplyPeerRest(*rest)
                                        && networkWorldPendingRestMinutes() == 0
                                        && game_time() == rest->gameTime;
                                    if (eventApplied) {
                                        restCompletionGameTime = rest->gameTime;
                                        restCompletionInterrupted = rest->interrupted;
                                    }
                                }
                            } else if (event
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
                            } else if (event && event.event.sequence == expectedEventSequence
                                && event.event.causedBy == scenarioCommand.sequence
                                && smokeScenario == SmokeScenario::MapTransition) {
                                const auto* elevator = std::get_if<ElevatorTransitionedEvent>(&event.event.payload);
                                eventApplied = elevator != nullptr
                                    && elevator->actorId == scenarioCommand.actorId
                                    && elevator->elevatorType == mapTransitionFixture->elevatorType
                                    && elevator->map == mapTransitionFixture->destinationMap
                                    && elevator->hostElevation == mapTransitionFixture->destinationElevation
                                    && elevator->guestElevation == mapTransitionFixture->destinationElevation
                                    && networkWorldApplyPeerElevator(*elevator);
                            } else if (event && event.event.sequence == expectedEventSequence
                                && event.event.causedBy == scenarioCommand.sequence
                                && smokeScenario == SmokeScenario::ExitGrid) {
                                const auto* exitGrid = std::get_if<ExitGridTransitionedEvent>(&event.event.payload);
                                eventApplied = exitGrid != nullptr
                                    && exitGrid->actorId == scenarioCommand.actorId
                                    && exitGrid->exitId == exitGridFixture->exitId
                                    && exitGrid->map == exitGridFixture->destinationMap
                                    && exitGrid->placements.size() == 2
                                    && networkWorldApplyPeerExitGrid(*exitGrid);
                            } else if (event && event.event.sequence == expectedEventSequence
                                && event.event.causedBy == scenarioCommand.sequence
                                && isSceneryTransitionSmokeScenario()) {
                                const auto* transition = std::get_if<SceneryTransitionedEvent>(&event.event.payload);
                                eventApplied = transition != nullptr
                                    && transition->actorId == scenarioCommand.actorId
                                    && transition->transitionId == sceneryTransitionFixture->transitionId
                                    && transition->map == sceneryTransitionFixture->map
                                    && transition->placements.size() == 2
                                    && networkWorldApplyPeerSceneryTransition(*transition);
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
                                stateConverged = stateConverged
                                    && worldMapStateMatchesFixture(localState.worldMap);
                                if (stateConverged && smokeScenario == SmokeScenario::Quest) {
                                    stateConverged = networkWorldVerifyQuestSmokeTest(*questFixture);
                                } else if (stateConverged && smokeScenario == SmokeScenario::Elevation) {
                                    stateConverged = networkWorldVerifyElevatorSmokeTest(*elevatorFixture);
                                } else if (stateConverged && smokeScenario == SmokeScenario::MapTransition) {
                                    stateConverged = networkWorldVerifyMapTransitionSmokeTest(*mapTransitionFixture);
                                } else if (stateConverged && smokeScenario == SmokeScenario::ExitGrid) {
                                    stateConverged = networkWorldVerifyExitGridSmokeTest(*exitGridFixture);
                                } else if (stateConverged && isSceneryTransitionSmokeScenario()) {
                                    stateConverged = networkWorldVerifySceneryTransitionSmokeTest(*sceneryTransitionFixture);
                                } else if (stateConverged && smokeScenario == SmokeScenario::Rest) {
                                    stateConverged = restTimeReached(restCompletionGameTime)
                                        && restCompletionInterrupted == smokeRestInterrupt
                                        && game_time() == restCompletionGameTime
                                        && networkWorldPendingRestMinutes() == 0
                                        && networkWorldPhase() == SessionPhase::Exploration
                                        && restHealed();
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
                            } else if (smokeScenario == SmokeScenario::MapTransition) {
                                const auto* elevator = std::get_if<ElevatorCommand>(&command.command.payload);
                                payloadMatches = elevator != nullptr
                                    && elevator->elevatorType == mapTransitionFixture->elevatorType
                                    && elevator->destinationLevel == mapTransitionFixture->destinationLevel;
                            } else if (smokeScenario == SmokeScenario::ExitGrid) {
                                const auto* exitGrid = std::get_if<ExitGridCommand>(&command.command.payload);
                                payloadMatches = exitGrid != nullptr
                                    && exitGrid->exitId == exitGridFixture->exitId;
                            } else if (isSceneryTransitionSmokeScenario()) {
                                const auto* transition = std::get_if<SceneryTransitionCommand>(&command.command.payload);
                                payloadMatches = transition != nullptr
                                    && transition->transitionId == sceneryTransitionFixture->transitionId;
                            } else if (smokeScenario == SmokeScenario::Rest) {
                                const auto* rest = std::get_if<RestCommand>(&command.command.payload);
                                payloadMatches = rest != nullptr && rest->minutes == smokeRestMinutes;
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
                            std::fprintf(stderr,
                                "Smoke command mismatch: payload=%d type=%zu/%zu sequence=%llu/%llu player=%u/%u actor=%u/%u phase=%d/%d revision=%u/%u.\n",
                                payloadMatches,
                                command.command.payload.index(),
                                scenarioCommand.payload.index(),
                                static_cast<unsigned long long>(command.command.sequence.value),
                                static_cast<unsigned long long>(scenarioCommand.sequence.value),
                                command.command.playerId.value,
                                scenarioCommand.playerId.value,
                                command.command.actorId.value,
                                scenarioCommand.actorId.value,
                                static_cast<int>(command.command.expectedPhase),
                                static_cast<int>(scenarioCommand.expectedPhase),
                                command.command.expectedPhaseRevision,
                                scenarioCommand.expectedPhaseRevision);
                            if (const auto* receivedMove = std::get_if<MoveCommand>(&command.command.payload)) {
                                const auto* expectedMove = std::get_if<MoveCommand>(&scenarioCommand.payload);
                                if (expectedMove != nullptr) {
                                    std::fprintf(stderr,
                                        "Smoke move mismatch: tile=%d/%d elevation=%d/%d running=%d/%d.\n",
                                        receivedMove->destinationTile, expectedMove->destinationTile,
                                        receivedMove->elevation, expectedMove->elevation,
                                        receivedMove->running, expectedMove->running);
                                }
                            }
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
                        case SmokeScenario::MapTransition:
                            return networkWorldVerifyMapTransitionSmokeTest(*mapTransitionFixture);
                        case SmokeScenario::ExitGrid:
                            return networkWorldVerifyExitGridSmokeTest(*exitGridFixture);
                        case SmokeScenario::Stairs:
                        case SmokeScenario::TypedStairsIndependent:
                        case SmokeScenario::TypedStairsCrossMap:
                            return networkWorldVerifySceneryTransitionSmokeTest(*sceneryTransitionFixture);
                        case SmokeScenario::Rest:
                            return restTimeReached(game_time())
                                && networkWorldPendingRestMinutes() == 0
                                && networkWorldPhase() == SessionPhase::Exploration
                                && restHealed();
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
                    if (smokeWorldMapState) {
                        WorldMapState changedWorldMap = initialWorldMap;
                        changedWorldMap.specialEncounters ^= 1;
                        changedWorldMap.grid[42] = changedWorldMap.grid[42] == 0 ? 1 : 0;
                        changedWorldMap.knownTownEntrances[4]
                            = changedWorldMap.knownTownEntrances[4] == 0 ? 1 : 0;
                        changedWorldMap.x = 1325;
                        changedWorldMap.y = 325; // Two city-terrain pixels move without a time tick.
                        int savedVaultWater = game_global_vars[GVAR_VAULT_WATER];
                        int savedVatsCountdown = game_global_vars[GVAR_VATS_COUNTDOWN];
                        int savedMasterCountdown = game_global_vars[GVAR_COUNTDOWN_TO_DESTRUCTION];
                        game_global_vars[GVAR_VAULT_WATER] = 1;
                        game_global_vars[GVAR_VATS_COUNTDOWN] = 0;
                        game_global_vars[GVAR_COUNTDOWN_TO_DESTRUCTION] = 0;
                        int beforeTravelTime = game_time();
                        bool travelStarted = worldmap_apply_state(changedWorldMap)
                            && worldmap_authoritative_travel_begin(1328, 325);
                        WorldMapTravelStepResult travelStep = worldmap_authoritative_travel_step();
                        worldmap_authoritative_travel_cancel();
                        game_global_vars[GVAR_VAULT_WATER] = savedVaultWater;
                        game_global_vars[GVAR_VATS_COUNTDOWN] = savedVatsCountdown;
                        game_global_vars[GVAR_COUNTDOWN_TO_DESTRUCTION] = savedMasterCountdown;
                        objectMutated = objectMutated && travelStarted
                            && travelStep.status == WorldMapTravelStepStatus::Moving
                            && travelStep.x == 1327 && travelStep.y == 325
                            && travelStep.gameTime == beforeTravelTime;
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
                        if (smokeScenario == SmokeScenario::Rest) {
                            const auto* rest = std::get_if<RestStateChangedEvent>(&outcome.event->payload);
                            if (rest != nullptr && rest->completed) {
                                restCompletionGameTime = rest->gameTime;
                                restCompletionInterrupted = rest->interrupted;
                            }
                        }
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
                    bool mapTransitionCompleted = smokeScenario != SmokeScenario::MapTransition
                        || networkWorldVerifyMapTransitionSmokeTest(*mapTransitionFixture);
                    bool exitGridCompleted = smokeScenario != SmokeScenario::ExitGrid
                        || networkWorldVerifyExitGridSmokeTest(*exitGridFixture);
                    bool sceneryTransitionCompleted = !isSceneryTransitionSmokeScenario()
                        || networkWorldVerifySceneryTransitionSmokeTest(*sceneryTransitionFixture);
                    bool restCompleted = smokeScenario != SmokeScenario::Rest
                        || (restTimeReached(restCompletionGameTime)
                            && restCompletionInterrupted == smokeRestInterrupt
                            && game_time() == restCompletionGameTime
                            && networkWorldPendingRestMinutes() == 0
                            && networkWorldPhase() == SessionPhase::Exploration
                            && restHealed());
                    bool captured = accepted
                        && scenarioCompleted
                        && questCompleted
                        && elevationCompleted
                        && mapTransitionCompleted
                        && exitGridCompleted
                        && sceneryTransitionCompleted
                        && restCompleted
                        && objectMutated
                        && authoritativeEvents.size() == scenarioFinalEventSequence.value
                        && networkWorldCaptureAuthoritativeState(scenarioFinalEventSequence, state)
                        && worldMapStateMatchesFixture(state.worldMap);
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
                        if (smokeScenario == SmokeScenario::Rest) {
                            Object* restHost = networkWorldPlayerActor(kHostPlayerId);
                            Object* restGuest = networkWorldPlayerActor(kGuestPlayerId);
                            std::fprintf(stderr,
                                "Multiplayer rest: time=%d start=%d expected_delta=%d event_time=%d pending=%d phase=%d host_hp=%d/%d guest_hp=%d/%d.\n",
                                game_time(),
                                restStartingGameTime,
                                restExpectedMinutes * 600,
                                restCompletionGameTime,
                                networkWorldPendingRestMinutes(),
                                static_cast<int>(networkWorldPhase()),
                                restHost != nullptr ? critter_get_hits(restHost) : -1,
                                restHostInitialHits,
                                restGuest != nullptr ? critter_get_hits(restGuest) : -1,
                                restGuestInitialHits);
                        }
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
            if (isAuthorityProbedSmokeScenario()) {
                scenarioProbeCounts = engineExecutionProbeEnd();
                scenarioAuthorityPassed = launchOptions.mode == NetworkLaunchMode::Host
                    ? ((smokeScenario != SmokeScenario::Quest && !isSceneryTransitionSmokeScenario())
                            || scenarioProbeCounts.scriptProcedures > 0)
                        && scenarioProbeCounts.combatAttacks == 0
                    : scenarioProbeCounts.scriptProcedures == 0
                        && scenarioProbeCounts.combatAttacks == 0
                        && scenarioProbeCounts.randomDraws == 0;
            }

            if (!gameplayPassed || !scenarioAuthorityPassed) {
                if (runtimeStatus.find("SMOKE TEST FAILED") == std::string::npos) {
                    setStatus(!scenarioAuthorityPassed
                            ? "MULTIPLAYER SMOKE TEST FAILED: SCENARIO AUTHORITY PROBE"
                            : "MULTIPLAYER SMOKE TEST FAILED: GAMEPLAY WIRE EXCHANGE");
                }
                break;
            }

            if (isAuthorityProbedSmokeScenario()) {
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
    // Peer actor rebinding temporarily clears peerActor while the destination
    // map loads. Script/queue authority must remain disabled through that gap.
    return launchOptions.mode == NetworkLaunchMode::Join
        && networkWorldReplicaSessionActive();
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
    if (!networkWorldActive() || !networkRuntimeConnected()) {
        return false;
    }

    // A world-map proposal is an explicit consent request, not a local map
    // open. Keep the prompt in the ordinary game loop so both peers can answer
    // while exploration is still running.
    std::optional<PlayerId> proposer = networkWorldPendingWorldMapProposer();
    PlayerId localPlayerId = launchOptions.mode == NetworkLaunchMode::Host
        ? kHostPlayerId
        : kGuestPlayerId;
    if (proposer != announcedWorldMapProposer) {
        announcedWorldMapProposer = proposer;
        if (proposer.has_value()) {
            std::string message = *proposer == localPlayerId
                ? "World-map travel proposed. Waiting for the other player (N withdraws)."
                : playerLabel(*proposer) + " proposes world-map travel. Y accepts; N declines.";
            display_print(message.data());
        }
    }
    if (proposer.has_value()
        && (keyCode == KEY_UPPERCASE_Y || keyCode == KEY_LOWERCASE_Y
            || keyCode == KEY_UPPERCASE_N || keyCode == KEY_LOWERCASE_N)) {
        if (*proposer != localPlayerId
            || keyCode == KEY_UPPERCASE_N || keyCode == KEY_LOWERCASE_N) {
            bool accept = keyCode == KEY_UPPERCASE_Y || keyCode == KEY_LOWERCASE_Y;
            if (!networkRuntimeRequestSharedModal(SharedModalKind::WorldMap, accept)) {
                char failure[] = "World-map travel response could not be sent.";
                display_print(failure);
            }
        }
        return true;
    }
    if (keyCode != KEY_RETURN) {
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

    presentGameChatMessage(LobbyChatMessage { localPlayerId, std::move(text) }, "outgoing");
    return true;
}

bool networkRuntimeHandleCombatInput(int keyCode)
{
    if (launchOptions.mode != NetworkLaunchMode::Join
        || !networkWorldActive()
        || networkWorldPhase() != SessionPhase::Combat
        || keyCode != KEY_SPACE) {
        return false;
    }
    std::uint64_t revision = networkWorldCombatTurnRevision();
    if (networkWorldActiveCombatOwner() != kGuestPlayerId || revision == 0) {
        char message[] = "It is not your combat turn.";
        display_print(message);
    } else if (lastSentCombatEndRevision != revision) {
        if (lobby.sendLocalEndTurn(revision, networkWorldPhaseRevision())) {
            lastSentCombatEndRevision = revision;
        } else {
            char message[] = "Combat end-turn could not be sent.";
            display_print(message);
        }
    }
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
    lastSentCombatEndRevision = 0;
    nextAuthoritativeState = {};
    nextAgentWorldReport = {};
    pendingLocalExitGrid = networkWorldReadyLocalExitGrid();
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

bool networkRuntimeHandleLocalSceneryTransition(Object* target)
{
    if (!networkWorldActive() || target == nullptr || FID_TYPE(target->fid) != OBJ_TYPE_SCENERY) {
        return false;
    }
    Proto* proto = nullptr;
    if (proto_ptr(target->pid, &proto) == -1
        || (proto->scenery.type != SCENERY_TYPE_STAIRS
            && proto->scenery.type != SCENERY_TYPE_LADDER_UP
            && proto->scenery.type != SCENERY_TYPE_LADDER_DOWN)) {
        return false;
    }
    std::optional<EntityId> transitionId = networkWorldFindEntity(target);
    if (!transitionId.has_value()) {
        debug_printf("Multiplayer scenery transition is missing a shared entity ID.\n");
        return true;
    }
    if (!networkRuntimeSubmitLocalSceneryTransition(*transitionId)) {
        debug_printf("Multiplayer scenery transition command could not be submitted.\n");
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

bool networkRuntimeSubmitLocalExitGrid(EntityId exitId)
{
    if (!networkWorldActive()
        || networkWorldPhase() != SessionPhase::Exploration
        || !isValid(exitId)) {
        return false;
    }
    bool worldMapExit = networkWorldIsWorldMapExitGrid(exitId);
    bool submitted = launchOptions.mode == NetworkLaunchMode::Host
        ? submitHostCommand(ExitGridCommand { exitId })
        : lobby.sendLocalExitGrid(exitId, networkWorldPhaseRevision());
    if (submitted && (launchOptions.mode == NetworkLaunchMode::Join || worldMapExit)) {
        pendingLocalExitGrid = exitId;
    } else if (launchOptions.mode == NetworkLaunchMode::Host) {
        pendingLocalExitGrid.reset();
    }
    return submitted;
}

bool networkRuntimeSubmitLocalSceneryTransition(EntityId transitionId)
{
    if (!networkWorldActive()
        || networkWorldPhase() != SessionPhase::Exploration
        || !isValid(transitionId)) {
        return false;
    }
    return launchOptions.mode == NetworkLaunchMode::Host
        ? submitHostCommand(SceneryTransitionCommand { transitionId })
        : lobby.sendLocalSceneryTransition(transitionId, networkWorldPhaseRevision());
}

bool networkRuntimeSubmitLocalRest(std::int32_t minutes)
{
    if (!networkWorldActive()
        || lobby.state() != NetworkLobbyState::Ready
        || networkWorldPhase() != SessionPhase::Exploration
        || !isValidRestMinutes(minutes)) {
        return false;
    }
    if (launchOptions.mode == NetworkLaunchMode::Host && pipboy_is_open()) {
        pendingLocalRestRequest = minutes;
        return true;
    }
    return launchOptions.mode == NetworkLaunchMode::Host
        ? submitHostCommand(RestCommand { minutes })
        : lobby.sendLocalRest(minutes, networkWorldPhaseRevision());
}

bool networkRuntimeSharedRestEnabled()
{
    return networkWorldActive();
}

std::int32_t networkRuntimePendingRestMinutes()
{
    return networkWorldActive() ? networkWorldPendingRestMinutes() : 0;
}

std::string networkRuntimePendingRestProposerName()
{
    return networkWorldActive() ? networkWorldPendingRestProposerName() : std::string {};
}

bool networkRuntimeLocalRestProposal()
{
    return networkWorldActive() && networkWorldLocalRestProposal();
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
        || (!open && kind != SharedModalKind::WorldMap && networkWorldPhase() != sharedModalPhase(kind))) {
        return false;
    }
    return launchOptions.mode == NetworkLaunchMode::Host
        ? submitHostCommand(SharedModalCommand { kind, open })
        : lobby.sendLocalSharedModal(kind, open, networkWorldPhase(), networkWorldPhaseRevision());
}

bool networkRuntimeSubmitLocalWorldMapRoute(std::int32_t targetX, std::int32_t targetY, bool clear)
{
    WorldMapRouteCommand route { targetX, targetY, clear };
    if (!networkWorldLocalWorldMapController() || !isValid(route)) {
        return false;
    }
    return launchOptions.mode == NetworkLaunchMode::Host
        ? submitHostCommand(route)
        : lobby.sendLocalWorldMapRoute(route, networkWorldPhaseRevision());
}

void networkRuntimeFlushWorldMapTerminalEvent()
{
    if (launchOptions.mode != NetworkLaunchMode::Host || !networkWorldActive()) return;
    for (int attempt = 0; attempt < 20; attempt++) {
        networkRuntimeBackgroundProcess();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
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
    return networkWorldActive()
        && (networkWorldSharedModalActive()
            || (networkRuntimeIsGuestReplica()
                && networkWorldPhase() == SessionPhase::Combat));
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
    lastSentCombatEndRevision = 0;
    pendingRecoveryRequest.reset();
    pendingLocalItemDrop.reset();
    pendingLocalExitGrid.reset();
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
    pendingLocalExitGrid.reset();
    nextAuthoritativeState = {};
    nextAgentWorldReport = {};
    announcedWorldMapProposer.reset();
}

} // namespace multiplayer
} // namespace fallout
