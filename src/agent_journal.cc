#include "agent_journal.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <utility>

namespace fallout {
namespace {

FILE* journalStream = nullptr;
std::string journalPath;
std::string lastWorldState;
std::uint64_t journalSequence = 0;
bool worldWasActive = false;
bool closeRegistered = false;
std::mutex journalMutex;

std::string jsonString(const char* value)
{
    const unsigned char* cursor = reinterpret_cast<const unsigned char*>(value != nullptr ? value : "");
    std::ostringstream output;
    output << '"';
    while (*cursor != '\0') {
        unsigned char ch = *cursor++;
        switch (ch) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (ch >= 0x20 && ch <= 0x7E) {
                output << static_cast<char>(ch);
            } else {
                char escaped[7];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
                output << escaped;
            }
            break;
        }
    }
    output << '"';
    return output.str();
}

const char* booleanValue(bool value)
{
    return value ? "true" : "false";
}

std::uint64_t currentTimeMilliseconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

void writeRecordLocked(const char* event, const std::string& fields)
{
    if (journalStream == nullptr) {
        return;
    }
    std::fprintf(journalStream,
        "{\"seq\":%llu,\"time_ms\":%llu,\"event\":%s%s}\n",
        static_cast<unsigned long long>(++journalSequence),
        static_cast<unsigned long long>(currentTimeMilliseconds()),
        jsonString(event).c_str(),
        fields.c_str());
    std::fflush(journalStream);
}

std::string actorJson(const AgentJournalActorState& actor)
{
    std::ostringstream output;
    output << "{\"player_id\":" << actor.playerId
           << ",\"entity_id\":" << actor.entityId
           << ",\"name\":" << jsonString(actor.name.c_str())
           << ",\"local\":" << booleanValue(actor.local)
           << ",\"tile\":" << actor.tile
           << ",\"elevation\":" << actor.elevation
           << ",\"rotation\":" << actor.rotation
           << ",\"screen_x\":" << actor.screenX
           << ",\"screen_y\":" << actor.screenY
           << ",\"hp\":" << actor.hitPoints
           << ",\"ap\":" << actor.actionPoints
           << '}';
    return output.str();
}

std::string inventoryItemJson(const AgentJournalInventoryItemState& item)
{
    std::ostringstream output;
    output << "{\"entity_id\":" << item.entityId
           << ",\"pid\":" << item.pid
           << ",\"name\":" << jsonString(item.name.c_str())
           << ",\"quantity\":" << item.quantity
           << ",\"equipped\":" << booleanValue(item.equipped)
           << '}';
    return output.str();
}

std::string critterJson(const AgentJournalCritterState& critter)
{
    std::ostringstream output;
    output << "{\"entity_id\":" << critter.entityId
           << ",\"pid\":" << critter.pid
           << ",\"name\":" << jsonString(critter.name.c_str())
           << ",\"disposition\":" << jsonString(critter.disposition.c_str())
           << ",\"team\":" << critter.team
           << ",\"tile\":" << critter.tile
           << ",\"elevation\":" << critter.elevation
           << ",\"rotation\":" << critter.rotation
           << ",\"screen_x\":" << critter.screenX
           << ",\"screen_y\":" << critter.screenY
           << ",\"distance\":" << critter.distance
           << ",\"hp\":" << critter.hitPoints
           << '}';
    return output.str();
}

std::string interactableJson(const AgentJournalInteractableState& interactable)
{
    std::ostringstream output;
    output << "{\"entity_id\":" << interactable.entityId
           << ",\"pid\":" << interactable.pid
           << ",\"kind\":" << jsonString(interactable.kind.c_str())
           << ",\"name\":" << jsonString(interactable.name.c_str())
           << ",\"tile\":" << interactable.tile
           << ",\"elevation\":" << interactable.elevation
           << ",\"screen_x\":" << interactable.screenX
           << ",\"screen_y\":" << interactable.screenY
           << ",\"distance\":" << interactable.distance
           << ",\"open\":" << booleanValue(interactable.open)
           << ",\"locked\":" << booleanValue(interactable.locked)
           << '}';
    return output.str();
}

} // namespace

bool agentJournalConfigure(int argc, char** argv)
{
    std::string configuredPath;
    constexpr const char* prefix = "--agent-journal=";
    for (int index = 1; index < argc; index++) {
        std::string argument = argv[index] != nullptr ? argv[index] : "";
        std::string candidate;
        if (argument.rfind(prefix, 0) == 0) {
            candidate = argument.substr(std::strlen(prefix));
        } else if (argument == "--agent-journal") {
            if (index + 1 >= argc || argv[index + 1] == nullptr) {
                std::fprintf(stderr, "--agent-journal requires a file path.\n");
                return false;
            }
            candidate = argv[++index];
        } else {
            continue;
        }
        if (candidate.empty() || candidate.rfind("--", 0) == 0 || !configuredPath.empty()) {
            std::fprintf(stderr, "--agent-journal requires one non-empty file path.\n");
            return false;
        }
        configuredPath = std::move(candidate);
    }

    if (configuredPath.empty()) {
        const char* environmentPath = std::getenv("FALLOUT_AGENT_JOURNAL");
        if (environmentPath != nullptr) {
            configuredPath = environmentPath;
        }
    }
    if (configuredPath.empty()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(journalMutex);
    if (journalStream != nullptr) {
        std::fclose(journalStream);
        journalStream = nullptr;
    }
    journalStream = std::fopen(configuredPath.c_str(), "wt");
    if (journalStream == nullptr) {
        std::fprintf(stderr, "Could not open agent journal: %s.\n", configuredPath.c_str());
        return false;
    }
    journalPath = configuredPath;
    journalSequence = 0;
    lastWorldState.clear();
    worldWasActive = false;
    writeRecordLocked("session_start", "");
    if (!closeRegistered) {
        std::atexit(agentJournalClose);
        closeRegistered = true;
    }
    return true;
}

bool agentJournalEnabled()
{
    std::lock_guard<std::mutex> lock(journalMutex);
    return journalStream != nullptr;
}

const char* agentJournalPath()
{
    return journalPath.c_str();
}

void agentJournalWriteText(const char* event, const char* text)
{
    std::lock_guard<std::mutex> lock(journalMutex);
    writeRecordLocked(event, ",\"text\":" + jsonString(text));
}

void agentJournalWriteNamedText(const char* event, const char* name, const char* text)
{
    std::lock_guard<std::mutex> lock(journalMutex);
    writeRecordLocked(event,
        ",\"name\":" + jsonString(name)
            + ",\"text\":" + jsonString(text));
}

void agentJournalWriteChat(const char* direction,
    std::uint32_t playerId,
    const char* playerName,
    const char* text)
{
    std::lock_guard<std::mutex> lock(journalMutex);
    writeRecordLocked("chat",
        ",\"direction\":" + jsonString(direction)
            + ",\"player_id\":" + std::to_string(playerId)
            + ",\"player_name\":" + jsonString(playerName)
            + ",\"text\":" + jsonString(text));
}

void agentJournalWriteDialogueOption(int index, const char* text)
{
    std::lock_guard<std::mutex> lock(journalMutex);
    writeRecordLocked("dialogue_option",
        ",\"index\":" + std::to_string(index)
            + ",\"text\":" + jsonString(text));
}

void agentJournalWriteAgentCommand(std::uint64_t commandId,
    const char* command,
    const char* status,
    const char* message)
{
    std::lock_guard<std::mutex> lock(journalMutex);
    writeRecordLocked("agent_command",
        ",\"command_id\":" + std::to_string(commandId)
            + ",\"command\":" + jsonString(command)
            + ",\"status\":" + jsonString(status)
            + ",\"message\":" + jsonString(message));
}

void agentJournalWriteWorldState(const AgentJournalWorldState& state)
{
    std::ostringstream fields;
    fields << ",\"map\":" << jsonString(state.map.c_str())
           << ",\"phase\":" << jsonString(state.phase.c_str())
           << ",\"connected\":" << booleanValue(state.connected)
           << ",\"combat\":" << booleanValue(state.combat)
           << ",\"pending_rest\":";
    if (state.pendingRestMinutes > 0) {
        fields << "{\"minutes\":" << state.pendingRestMinutes
               << ",\"proposer_player_id\":" << state.pendingRestProposerId
               << ",\"proposer_name\":" << jsonString(state.pendingRestProposerName.c_str())
               << '}';
    } else {
        fields << "null";
    }
    fields << ",\"host\":" << actorJson(state.host)
           << ",\"guest\":" << actorJson(state.guest)
           << ",\"local_inventory\":[";
    for (std::size_t index = 0; index < state.localInventory.size(); index++) {
        if (index != 0) {
            fields << ',';
        }
        fields << inventoryItemJson(state.localInventory[index]);
    }
    fields << "],\"visible_critters\":[";
    for (std::size_t index = 0; index < state.visibleCritters.size(); index++) {
        if (index != 0) {
            fields << ',';
        }
        fields << critterJson(state.visibleCritters[index]);
    }
    fields << "],\"visible_interactables\":[";
    for (std::size_t index = 0; index < state.visibleInteractables.size(); index++) {
        if (index != 0) {
            fields << ',';
        }
        fields << interactableJson(state.visibleInteractables[index]);
    }
    fields << ']';
    std::string encoded = fields.str();

    std::lock_guard<std::mutex> lock(journalMutex);
    if (encoded == lastWorldState && worldWasActive) {
        return;
    }
    lastWorldState = encoded;
    worldWasActive = true;
    writeRecordLocked("world_state", encoded);
}

void agentJournalWriteWorldExit()
{
    std::lock_guard<std::mutex> lock(journalMutex);
    if (!worldWasActive) {
        return;
    }
    worldWasActive = false;
    lastWorldState.clear();
    writeRecordLocked("world_exit", "");
}

void agentJournalClose()
{
    std::lock_guard<std::mutex> lock(journalMutex);
    if (journalStream == nullptr) {
        return;
    }
    writeRecordLocked("session_end", "");
    std::fclose(journalStream);
    journalStream = nullptr;
}

} // namespace fallout
