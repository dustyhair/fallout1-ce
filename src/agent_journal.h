#ifndef FALLOUT_AGENT_JOURNAL_H_
#define FALLOUT_AGENT_JOURNAL_H_

#include <cstdint>
#include <string>
#include <vector>

namespace fallout {

struct AgentJournalActorState {
    std::uint32_t playerId = 0;
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

struct AgentJournalWorldState {
    std::string map;
    std::string phase;
    bool connected = false;
    bool combat = false;
    AgentJournalActorState host;
    AgentJournalActorState guest;
    std::vector<AgentJournalCritterState> visibleCritters;
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
