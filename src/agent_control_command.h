#ifndef FALLOUT_AGENT_CONTROL_COMMAND_H_
#define FALLOUT_AGENT_CONTROL_COMMAND_H_

#include <cstdint>
#include <string>

namespace fallout {

enum class AgentControlCommandType {
    Move,
    LeftClick,
    RightClick,
    Key,
    Text,
};

struct AgentControlCommand {
    std::uint64_t id = 0;
    AgentControlCommandType type = AgentControlCommandType::Move;
    int x = 0;
    int y = 0;
    int keyCode = -1;
    std::string text;
};

bool agentControlParseCommand(const std::string& line,
    AgentControlCommand& command,
    std::string& error);
const char* agentControlCommandTypeName(AgentControlCommandType type);

} // namespace fallout

#endif /* FALLOUT_AGENT_CONTROL_COMMAND_H_ */
