#include "agent_control.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_set>
#include <utility>

#include "agent_control_command.h"
#include "agent_journal.h"
#include "multiplayer/local_player_context.h"
#include "multiplayer/network_runtime.h"
#include "multiplayer/network_world.h"
#include "plib/gnw/input.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/svga.h"

namespace fallout {
namespace {

constexpr std::size_t kMaximumBufferedInput = 64 * 1024;
constexpr std::size_t kMaximumPendingCommands = 256;
constexpr std::size_t kRememberedCommandIds = 4096;

FILE* commandStream = nullptr;
std::string commandPath;
std::string incomingData;
std::deque<std::string> pendingLines;
std::unordered_set<std::uint64_t> commandIds;
std::deque<std::uint64_t> commandIdOrder;
std::string pendingText;
std::size_t pendingTextIndex = 0;
std::uint64_t pendingTextId = 0;
std::uint64_t pendingClickId = 0;
AgentControlCommandType pendingClickType = AgentControlCommandType::LeftClick;
int pendingClickX = 0;
int pendingClickY = 0;
int pendingClickPhase = 0;
bool started = false;
bool closeRegistered = false;

void rememberCommandId(std::uint64_t id)
{
    commandIds.insert(id);
    commandIdOrder.push_back(id);
    if (commandIdOrder.size() > kRememberedCommandIds) {
        commandIds.erase(commandIdOrder.front());
        commandIdOrder.pop_front();
    }
}

void queueCompleteLines()
{
    std::size_t newline;
    while (pendingLines.size() < kMaximumPendingCommands
        && (newline = incomingData.find('\n')) != std::string::npos) {
        std::string line = incomingData.substr(0, newline);
        incomingData.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line[first] != '#') {
            pendingLines.push_back(std::move(line));
        }
    }
}

void readAvailableInput()
{
    char buffer[2048];
    queueCompleteLines();
    clearerr(commandStream);
    while (pendingLines.size() < kMaximumPendingCommands) {
        std::size_t bytesRead = std::fread(buffer, 1, sizeof(buffer), commandStream);
        if (bytesRead == 0) {
            clearerr(commandStream);
            break;
        }
        incomingData.append(buffer, bytesRead);
        if (incomingData.size() > kMaximumBufferedInput) {
            incomingData.clear();
            agentJournalWriteAgentCommand(0, "input", "rejected", "command input buffer exceeded 64 KiB");
            break;
        }
        queueCompleteLines();
    }
}

bool coordinatesAreOnScreen(int x, int y)
{
    return x >= 0 && y >= 0 && x < screenGetWidth() && y < screenGetHeight();
}

void executeCommand(const AgentControlCommand& command)
{
    const char* commandName = agentControlCommandTypeName(command.type);
    if (commandIds.find(command.id) != commandIds.end()) {
        agentJournalWriteAgentCommand(command.id, commandName, "rejected", "duplicate command id");
        return;
    }
    rememberCommandId(command.id);

    switch (command.type) {
    case AgentControlCommandType::Move:
        if (!coordinatesAreOnScreen(command.x, command.y)) {
            agentJournalWriteAgentCommand(command.id, commandName, "rejected", "coordinates are outside the game screen");
            return;
        }
        mouse_set_position(command.x, command.y);
        agentJournalWriteAgentCommand(command.id, commandName, "executed", "pointer moved");
        return;
    case AgentControlCommandType::LeftClick:
    case AgentControlCommandType::RightClick:
        if (!coordinatesAreOnScreen(command.x, command.y)) {
            agentJournalWriteAgentCommand(command.id, commandName, "rejected", "coordinates are outside the game screen");
            return;
        }
        if (mouse_hidden()) {
            agentJournalWriteAgentCommand(command.id, commandName, "rejected", "game pointer is hidden");
            return;
        }
        mouse_set_position(command.x, command.y);
        pendingClickId = command.id;
        pendingClickType = command.type;
        pendingClickX = command.x;
        pendingClickY = command.y;
        pendingClickPhase = 1;
        agentJournalWriteAgentCommand(command.id, commandName, "accepted", "pointer positioned; click pending");
        return;
    case AgentControlCommandType::Key:
        GNW_add_input_buffer(command.keyCode);
        agentJournalWriteAgentCommand(command.id, commandName, "executed", "key injected");
        return;
    case AgentControlCommandType::Text:
        pendingText = command.text;
        pendingTextIndex = 0;
        pendingTextId = command.id;
        agentJournalWriteAgentCommand(command.id, commandName, "accepted", "text injection started");
        return;
    case AgentControlCommandType::GameMove:
        if ((multiplayer::networkWorldPhase() != multiplayer::SessionPhase::Combat
                || multiplayer::networkWorldActiveCombatOwner()
                    == multiplayer::localPlayerId())
            && multiplayer::networkRuntimeSubmitLocalMove(
                command.tile, command.elevation, command.running)) {
            agentJournalWriteAgentCommand(command.id, commandName, "accepted", "semantic movement submitted");
        } else {
            agentJournalWriteAgentCommand(command.id, commandName, "rejected", "semantic movement is unavailable or invalid");
        }
        return;
    case AgentControlCommandType::GameFace:
        if ((multiplayer::networkWorldPhase() != multiplayer::SessionPhase::Combat
                || multiplayer::networkWorldActiveCombatOwner()
                    == multiplayer::localPlayerId())
            && multiplayer::networkRuntimeSubmitLocalFacing(command.rotation)) {
            agentJournalWriteAgentCommand(command.id, commandName, "accepted", "semantic facing submitted");
        } else {
            agentJournalWriteAgentCommand(command.id, commandName, "rejected", "semantic facing is unavailable or invalid");
        }
        return;
    case AgentControlCommandType::GameAttack: {
        if (multiplayer::networkRuntimeSubmitCombatAttack(
                multiplayer::EntityId { command.entityId }, command.hitMode,
                command.hitLocation)) {
            agentJournalWriteAgentCommand(command.id, commandName,
                "accepted", "combat attack submitted");
        } else {
            agentJournalWriteAgentCommand(command.id, commandName,
                "rejected", "combat target or active turn is unavailable");
        }
        return;
    }
    case AgentControlCommandType::GameReload:
        if (multiplayer::networkRuntimeSubmitCombatReload(
                multiplayer::EntityId { command.entityId }, command.hitMode)) {
            agentJournalWriteAgentCommand(command.id, commandName,
                "accepted", "combat reload submitted");
        } else {
            agentJournalWriteAgentCommand(command.id, commandName,
                "rejected", "weapon or active turn is unavailable");
        }
        return;
    case AgentControlCommandType::GameCombatItem:
        if (multiplayer::networkRuntimeSubmitCombatItem(
                multiplayer::EntityId { command.entityId },
                multiplayer::EntityId { command.destinationEntityId })) {
            agentJournalWriteAgentCommand(command.id, commandName,
                "accepted", "combat item use submitted");
        } else {
            agentJournalWriteAgentCommand(command.id, commandName,
                "rejected", "item or active turn is unavailable");
        }
        return;
    case AgentControlCommandType::GameEndTurn:
        if (multiplayer::networkWorldPhase() == multiplayer::SessionPhase::Combat
            && multiplayer::networkWorldActiveCombatOwner()
                == multiplayer::localPlayerId()) {
            GNW_add_input_buffer(KEY_SPACE);
            agentJournalWriteAgentCommand(command.id, commandName,
                "accepted", "combat end-turn input queued");
        } else {
            agentJournalWriteAgentCommand(command.id, commandName,
                "rejected", "active combat turn is unavailable");
        }
        return;
    case AgentControlCommandType::GameDoor:
    case AgentControlCommandType::GamePickup:
    case AgentControlCommandType::GameLoot: {
        Object* target = multiplayer::networkWorldFindObject(multiplayer::EntityId { command.entityId });
        bool submitted = target != nullptr
            && (command.type == AgentControlCommandType::GameDoor
                    ? multiplayer::networkRuntimeHandleLocalDoorUse(target)
                    : command.type == AgentControlCommandType::GamePickup
                    ? multiplayer::networkRuntimeHandleLocalPickup(target)
                    : multiplayer::networkRuntimeHandleLocalLoot(target));
        if (submitted) {
            agentJournalWriteAgentCommand(command.id, commandName, "accepted", "semantic entity command submitted");
        } else {
            agentJournalWriteAgentCommand(command.id, commandName, "rejected", "entity is unavailable or invalid for this action");
        }
        return;
    }
    case AgentControlCommandType::GameSkill: {
        Object* target = multiplayer::networkWorldFindObject(multiplayer::EntityId { command.entityId });
        bool submitted = target != nullptr
            && multiplayer::networkRuntimeHandleLocalSkillUse(target, command.skill);
        if (submitted) {
            agentJournalWriteAgentCommand(command.id, commandName, "accepted", "semantic skill use submitted");
        } else {
            agentJournalWriteAgentCommand(command.id, commandName, "rejected", "skill or target is unavailable for this action");
        }
        return;
    }
    case AgentControlCommandType::GameUseItem: {
        Object* actor = multiplayer::localPlayerActor();
        Object* item = multiplayer::networkWorldFindObject(multiplayer::EntityId { command.itemEntityId });
        Object* target = multiplayer::networkWorldFindObject(multiplayer::EntityId { command.entityId });
        bool submitted = actor != nullptr
            && item != nullptr
            && target != nullptr
            && multiplayer::networkRuntimeHandleLocalItemUse(actor, item, target);
        if (submitted) {
            agentJournalWriteAgentCommand(command.id, commandName, "accepted", "semantic item use submitted");
        } else {
            agentJournalWriteAgentCommand(command.id, commandName, "rejected", "item or target is unavailable for this action");
        }
        return;
    }
    case AgentControlCommandType::GameElevator:
        if (multiplayer::networkRuntimeSubmitLocalElevator(
                command.elevatorType,
                command.elevatorLevel)) {
            agentJournalWriteAgentCommand(command.id, commandName, "accepted", "authoritative elevator transition submitted");
        } else {
            agentJournalWriteAgentCommand(command.id, commandName, "rejected", "elevator or destination is unavailable for this transition");
        }
        return;
    case AgentControlCommandType::GameExit:
        if (multiplayer::networkRuntimeSubmitLocalExitGrid(
                multiplayer::EntityId { command.entityId })) {
            agentJournalWriteAgentCommand(command.id, commandName, "accepted", "authoritative exit-grid transition submitted");
        } else {
            agentJournalWriteAgentCommand(command.id, commandName, "rejected", "exit grid is unavailable or the party is not ready");
        }
        return;
    case AgentControlCommandType::GameStairs:
        if (multiplayer::networkRuntimeSubmitLocalSceneryTransition(
                multiplayer::EntityId { command.entityId })) {
            agentJournalWriteAgentCommand(command.id, commandName, "accepted", "authoritative scenery transition submitted");
        } else {
            agentJournalWriteAgentCommand(command.id, commandName, "rejected", "stairs or ladder is unavailable for this player");
        }
        return;
    case AgentControlCommandType::GameRest:
        if (multiplayer::networkRuntimeSubmitLocalRest(command.restMinutes)) {
            agentJournalWriteAgentCommand(command.id, commandName, "accepted", "rest proposal, approval, or withdrawal submitted");
        } else {
            agentJournalWriteAgentCommand(command.id, commandName, "rejected", "rest is unavailable or invalid");
        }
        return;
    case AgentControlCommandType::GameTalk:
        if (multiplayer::networkRuntimeSubmitLocalTalk(
                multiplayer::EntityId { command.entityId })) {
            agentJournalWriteAgentCommand(command.id, commandName, "accepted",
                "dialogue request submitted");
        } else {
            agentJournalWriteAgentCommand(command.id, commandName, "rejected",
                "dialogue target or phase unavailable");
        }
        return;
    case AgentControlCommandType::GameVote:
        if (multiplayer::networkRuntimeSubmitDialogueVote(
                command.dialogueRevision,
                static_cast<std::uint8_t>(command.dialogueOption - 1))) {
            agentJournalWriteAgentCommand(command.id, commandName, "accepted",
                "revision-keyed dialogue vote submitted");
        } else {
            agentJournalWriteAgentCommand(command.id, commandName, "rejected",
                "dialogue revision or option unavailable");
        }
        return;
    case AgentControlCommandType::GameGive:
        if (multiplayer::networkRuntimeGiveItemToPlayer(
                multiplayer::EntityId { command.destinationEntityId },
                multiplayer::EntityId { command.entityId },
                command.quantity)) {
            agentJournalWriteAgentCommand(command.id, commandName, "accepted", "authoritative player gift submitted");
        } else {
            agentJournalWriteAgentCommand(command.id, commandName, "rejected", "player gift is unavailable or invalid");
        }
        return;
    case AgentControlCommandType::GameTradeBegin:
        if (multiplayer::networkRuntimeBeginDirectTrade(
                multiplayer::EntityId { command.entityId })) {
            agentJournalWriteAgentCommand(command.id, commandName, "accepted",
                "bilateral trade negotiation started");
        } else {
            agentJournalWriteAgentCommand(command.id, commandName, "rejected",
                "other player is unavailable or not adjacent");
        }
        return;
    case AgentControlCommandType::GameTradeOffer:
    case AgentControlCommandType::GameTradeConfirm:
    case AgentControlCommandType::GameTradeCancel: {
        multiplayer::DirectTradeCommand trade;
        trade.tradeId = command.tradeId;
        trade.revision = command.tradeRevision;
        if (command.type == AgentControlCommandType::GameTradeOffer) {
            trade.action = multiplayer::DirectTradeAction::SetOffer;
            trade.offer.caps = command.caps;
            if (command.itemEntityId != 0) {
                trade.offer.items.push_back({
                    multiplayer::EntityId { command.itemEntityId },
                    command.quantity,
                });
            }
        } else if (command.type == AgentControlCommandType::GameTradeConfirm) {
            trade.action = multiplayer::DirectTradeAction::Confirm;
        } else {
            trade.action = multiplayer::DirectTradeAction::Cancel;
        }
        if (multiplayer::networkRuntimeSubmitDirectTrade(trade)) {
            agentJournalWriteAgentCommand(command.id, commandName, "accepted",
                "revisioned trade action submitted");
        } else {
            agentJournalWriteAgentCommand(command.id, commandName, "rejected",
                "trade id, revision, offer, or phase is invalid");
        }
        return;
    }
    }
}

void processAgentInput()
{
    if (commandStream == nullptr) {
        return;
    }

    if (pendingClickPhase == 1) {
        const char* commandName = agentControlCommandTypeName(pendingClickType);
        if (mouse_hidden()) {
            agentJournalWriteAgentCommand(pendingClickId, commandName, "rejected", "game pointer became hidden");
            pendingClickPhase = 0;
            return;
        }
        // Fallout's button dispatcher requires a hover cycle before the down
        // event. The command's first cycle positions the pointer, this cycle
        // presses, and the following cycle preserves SDL's release event.
        mouse_set_position(pendingClickX, pendingClickY);
        mouse_simulate_input(0,
            0,
            pendingClickType == AgentControlCommandType::LeftClick
                ? MOUSE_STATE_LEFT_BUTTON_DOWN
                : MOUSE_STATE_RIGHT_BUTTON_DOWN);
        agentJournalWriteAgentCommand(pendingClickId, commandName, "executed", "click injected");
        pendingClickPhase = 2;
        return;
    }
    if (pendingClickPhase == 2) {
        pendingClickPhase = 0;
        return;
    }

    if (pendingTextIndex < pendingText.size()) {
        GNW_add_input_buffer(static_cast<unsigned char>(pendingText[pendingTextIndex++]));
        if (pendingTextIndex == pendingText.size()) {
            agentJournalWriteAgentCommand(pendingTextId, "text", "executed", "text injection completed");
            pendingText.clear();
            pendingTextIndex = 0;
            pendingTextId = 0;
        }
        return;
    }

    readAvailableInput();
    if (pendingLines.empty()) {
        return;
    }

    std::string line = std::move(pendingLines.front());
    pendingLines.pop_front();
    AgentControlCommand command;
    std::string error;
    if (!agentControlParseCommand(line, command, error)) {
        agentJournalWriteAgentCommand(command.id, "parse", "rejected", error.c_str());
        return;
    }
    executeCommand(command);
}

} // namespace

bool agentControlConfigure(int argc, char** argv)
{
    std::string configuredPath;
    constexpr const char* prefix = "--agent-command-file=";
    for (int index = 1; index < argc; index++) {
        std::string argument = argv[index] != nullptr ? argv[index] : "";
        std::string candidate;
        if (argument.rfind(prefix, 0) == 0) {
            candidate = argument.substr(std::strlen(prefix));
        } else if (argument == "--agent-command-file") {
            if (index + 1 >= argc || argv[index + 1] == nullptr) {
                std::fprintf(stderr, "--agent-command-file requires a file path.\n");
                return false;
            }
            candidate = argv[++index];
        } else {
            continue;
        }
        if (candidate.empty() || candidate.rfind("--", 0) == 0 || !configuredPath.empty()) {
            std::fprintf(stderr, "--agent-command-file requires one non-empty file path.\n");
            return false;
        }
        configuredPath = std::move(candidate);
    }

    if (configuredPath.empty()) {
        const char* environmentPath = std::getenv("FALLOUT_AGENT_COMMAND_FILE");
        if (environmentPath != nullptr) {
            configuredPath = environmentPath;
        }
    }
    if (configuredPath.empty()) {
        return true;
    }

    FILE* truncateStream = std::fopen(configuredPath.c_str(), "wb");
    if (truncateStream == nullptr) {
        std::fprintf(stderr, "Could not create agent command file: %s.\n", configuredPath.c_str());
        return false;
    }
    std::fclose(truncateStream);

    commandStream = std::fopen(configuredPath.c_str(), "rb");
    if (commandStream == nullptr) {
        std::fprintf(stderr, "Could not open agent command file: %s.\n", configuredPath.c_str());
        return false;
    }
    commandPath = configuredPath;
    if (!closeRegistered) {
        std::atexit(agentControlClose);
        closeRegistered = true;
    }
    return true;
}

bool agentControlEnabled()
{
    return commandStream != nullptr;
}

const char* agentControlPath()
{
    return commandPath.c_str();
}

void agentControlStart()
{
    if (commandStream == nullptr || started) {
        return;
    }
    set_input_process(processAgentInput);
    set_background_processing_when_inactive(true);
    started = true;
    agentJournalWriteAgentCommand(0, "control", "ready", commandPath.c_str());
}

void agentControlStop()
{
    if (!started) {
        return;
    }
    set_input_process(nullptr);
    started = false;
}

void agentControlClose()
{
    agentControlStop();
    if (commandStream != nullptr) {
        std::fclose(commandStream);
        commandStream = nullptr;
    }
    commandPath.clear();
    incomingData.clear();
    pendingLines.clear();
    pendingText.clear();
    pendingTextIndex = 0;
    pendingTextId = 0;
    commandIds.clear();
    commandIdOrder.clear();
    pendingClickId = 0;
    pendingClickPhase = 0;
}

} // namespace fallout
