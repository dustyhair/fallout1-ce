#ifndef FALLOUT_AGENT_JOURNAL_H_
#define FALLOUT_AGENT_JOURNAL_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fallout {

struct AgentJournalActorState {
    std::uint32_t playerId = 0;
    std::uint64_t entityId = 0;
    std::string name;
    bool local = false;
    int tile = -1;
    int elevation = 0;
    int rotation = 0;
    int screenX = -1;
    int screenY = -1;
    int hitPoints = 0;
    int actionPoints = 0;
};

struct AgentJournalInventoryItemState {
    std::uint64_t entityId = 0;
    int pid = -1;
    std::string name;
    std::uint32_t quantity = 0;
    bool equipped = false;
};

struct AgentJournalCritterState {
    std::uint64_t entityId = 0;
    int pid = -1;
    std::string name;
    std::string disposition;
    int team = 0;
    int tile = -1;
    int elevation = 0;
    int rotation = 0;
    int screenX = -1;
    int screenY = -1;
    int distance = 0;
    int hitPoints = 0;
};

struct AgentJournalInteractableState {
    std::uint64_t entityId = 0;
    int pid = -1;
    std::string kind;
    std::string name;
    int tile = -1;
    int elevation = 0;
    int screenX = -1;
    int screenY = -1;
    int distance = 0;
    bool open = false;
    bool locked = false;
};

struct AgentJournalWorldState {
    std::string map;
    std::string phase;
    bool connected = false;
    bool combat = false;
    int pendingRestMinutes = 0;
    std::uint32_t pendingRestProposerId = 0;
    std::string pendingRestProposerName;
    struct Dialogue {
        std::uint64_t revision = 0;
        std::uint32_t talkerPlayerId = 0;
        std::uint64_t targetId = 0;
        std::string reply;
        std::vector<std::string> options;
        struct Vote {
            std::uint32_t playerId = 0;
            int option = 0; // One-based; zero means abstaining.
            bool connected = false;
        };
        std::vector<Vote> votes;
    };
    std::optional<Dialogue> dialogue;
    struct SharedActivity {
        std::uint64_t id = 0;
        std::uint32_t sourcePlayerId = 0;
        std::string sourceName;
        std::string kind;
        int subject = 0;
        int value = 0;
        std::string text;
    };
    std::vector<SharedActivity> sharedActivity;
    AgentJournalActorState host;
    AgentJournalActorState guest;
    std::vector<AgentJournalInventoryItemState> localInventory;
    std::vector<AgentJournalCritterState> visibleCritters;
    std::vector<AgentJournalInteractableState> visibleInteractables;
};

bool agentJournalConfigure(int argc, char** argv);
bool agentJournalEnabled();
const char* agentJournalPath();
void agentJournalWriteText(const char* event, const char* text);
void agentJournalWriteNamedText(const char* event, const char* name, const char* text);
void agentJournalWriteChat(const char* direction,
    std::uint32_t playerId,
    const char* playerName,
    const char* text);
void agentJournalWriteDialogueOption(int index, const char* text);
void agentJournalWriteAgentCommand(std::uint64_t commandId,
    const char* command,
    const char* status,
    const char* message);
void agentJournalWriteWorldState(const AgentJournalWorldState& state);
void agentJournalWriteWorldExit();
void agentJournalClose();

} // namespace fallout

#endif /* FALLOUT_AGENT_JOURNAL_H_ */
