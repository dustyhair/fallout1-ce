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
    GameMove,
    GameFace,
    GameDoor,
    GamePickup,
    GameLoot,
    GameSkill,
    GameUseItem,
    GameElevator,
    GameExit,
    GameStairs,
    GameRest,
    GameGive,
};

struct AgentControlCommand {
    std::uint64_t id = 0;
    AgentControlCommandType type = AgentControlCommandType::Move;
    int x = 0;
    int y = 0;
    int keyCode = -1;
    int tile = -1;
    int elevation = -1;
    int rotation = -1;
    int skill = -1;
    int elevatorType = -1;
    int elevatorLevel = -1;
    int restMinutes = 0;
    std::uint32_t entityId = 0;
    std::uint32_t itemEntityId = 0;
    std::uint32_t destinationEntityId = 0;
    std::uint32_t quantity = 0;
    bool running = false;
    std::string text;
};

bool agentControlParseCommand(const std::string& line,
    AgentControlCommand& command,
    std::string& error);
const char* agentControlCommandTypeName(AgentControlCommandType type);

} // namespace fallout

#endif /* FALLOUT_AGENT_CONTROL_COMMAND_H_ */
