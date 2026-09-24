#include "agent_control_command.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>

#include "game/skill_defs.h"
#include "plib/gnw/kb.h"

namespace fallout {
namespace {

bool hasTrailingInput(std::istringstream& input)
{
    std::string trailing;
    return static_cast<bool>(input >> trailing);
}

int namedKeyCode(const std::string& value)
{
    std::string name = value;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (name == "escape" || name == "esc") {
        return KEY_ESCAPE;
    }
    if (name == "enter" || name == "return") {
        return KEY_RETURN;
    }
    if (name == "tab") {
        return KEY_TAB;
    }
    if (name == "space") {
        return KEY_SPACE;
    }
    if (name == "backspace") {
        return KEY_BACKSPACE;
    }
    if (name == "up") {
        return KEY_ARROW_UP;
    }
    if (name == "down") {
        return KEY_ARROW_DOWN;
    }
    if (name == "left") {
        return KEY_ARROW_LEFT;
    }
    if (name == "right") {
        return KEY_ARROW_RIGHT;
    }
    if (name == "home") {
        return KEY_HOME;
    }
    if (name == "end") {
        return KEY_END;
    }
    if (name == "page_up" || name == "pageup") {
        return KEY_PAGE_UP;
    }
    if (name == "page_down" || name == "pagedown") {
        return KEY_PAGE_DOWN;
    }
    if (name == "delete" || name == "del") {
        return KEY_DELETE;
    }
    if (value.size() == 1) {
        unsigned char ch = static_cast<unsigned char>(value[0]);
        if (ch >= 0x20 && ch <= 0x7E) {
            return ch;
        }
    }
    return -1;
}

bool parseCoordinates(std::istringstream& input, AgentControlCommand& command, std::string& error)
{
    if (!(input >> command.x >> command.y) || hasTrailingInput(input)) {
        error = "expected exactly two integer coordinates";
        return false;
    }
    if (command.x < 0 || command.y < 0) {
        error = "coordinates cannot be negative";
        return false;
    }
    return true;
}

bool parseEntityId(std::istringstream& input, AgentControlCommand& command, std::string& error)
{
    std::uint64_t value = 0;
    if (!(input >> value)
        || hasTrailingInput(input)
        || value == 0
        || value > std::numeric_limits<std::uint32_t>::max()) {
        error = "expected one positive 32-bit entity id";
        return false;
    }
    command.entityId = static_cast<std::uint32_t>(value);
    return true;
}

bool parseGive(std::istringstream& input, AgentControlCommand& command, std::string& error)
{
    std::uint64_t destination = 0;
    std::uint64_t item = 0;
    std::uint64_t quantity = 0;
    if (!(input >> destination >> item >> quantity)
        || hasTrailingInput(input)
        || destination == 0
        || item == 0
        || quantity == 0
        || destination > std::numeric_limits<std::uint32_t>::max()
        || item > std::numeric_limits<std::uint32_t>::max()
        || quantity > std::numeric_limits<std::uint32_t>::max()) {
        error = "expected: game_give <destination actor id> <item id> <positive quantity>";
        return false;
    }
    command.destinationEntityId = static_cast<std::uint32_t>(destination);
    command.entityId = static_cast<std::uint32_t>(item);
    command.quantity = static_cast<std::uint32_t>(quantity);
    return true;
}

bool parseItemUse(std::istringstream& input, AgentControlCommand& command, std::string& error)
{
    std::uint64_t item = 0;
    std::uint64_t target = 0;
    if (!(input >> item >> target)
        || hasTrailingInput(input)
        || item == 0
        || target == 0
        || item == target
        || item > std::numeric_limits<std::uint32_t>::max()
        || target > std::numeric_limits<std::uint32_t>::max()) {
        error = "expected: game_use_item <item id> <target id>";
        return false;
    }
    command.itemEntityId = static_cast<std::uint32_t>(item);
    command.entityId = static_cast<std::uint32_t>(target);
    return true;
}

bool parseElevator(std::istringstream& input, AgentControlCommand& command, std::string& error)
{
    if (!(input >> command.elevatorType >> command.elevatorLevel)
        || hasTrailingInput(input)
        || command.elevatorType < 0
        || command.elevatorType >= 12
        || command.elevatorLevel < 1
        || command.elevatorLevel > 4) {
        error = "expected: game_elevator <type 0-11> <level 1-4>";
        return false;
    }
    command.elevatorLevel--;
    return true;
}

bool parseSkill(std::istringstream& input, AgentControlCommand& command, std::string& error)
{
    std::string name;
    std::uint64_t target = 0;
    if (!(input >> name >> target)
        || hasTrailingInput(input)
        || target == 0
        || target > std::numeric_limits<std::uint32_t>::max()) {
        error = "expected: game_skill <first_aid|doctor|lockpick|steal|traps|science|repair> <entity id>";
        return false;
    }
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (name == "first_aid" || name == "firstaid") {
        command.skill = SKILL_FIRST_AID;
    } else if (name == "doctor") {
        command.skill = SKILL_DOCTOR;
    } else if (name == "lockpick") {
        command.skill = SKILL_LOCKPICK;
    } else if (name == "steal") {
        command.skill = SKILL_STEAL;
    } else if (name == "traps") {
        command.skill = SKILL_TRAPS;
    } else if (name == "science") {
        command.skill = SKILL_SCIENCE;
    } else if (name == "repair") {
        command.skill = SKILL_REPAIR;
    } else {
        error = "unknown targeted exploration skill";
        return false;
    }
    command.entityId = static_cast<std::uint32_t>(target);
    return true;
}

} // namespace

bool agentControlParseCommand(const std::string& line,
    AgentControlCommand& command,
    std::string& error)
{
    command = AgentControlCommand {};
    error.clear();

    std::istringstream input(line);
    std::string verb;
    if (!(input >> command.id >> verb)) {
        error = "expected: <id> <command> [arguments]";
        return false;
    }
    if (command.id == 0) {
        error = "command id must be greater than zero";
        return false;
    }

    std::transform(verb.begin(), verb.end(), verb.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (verb == "move") {
        command.type = AgentControlCommandType::Move;
        return parseCoordinates(input, command, error);
    }
    if (verb == "click" || verb == "left_click") {
        command.type = AgentControlCommandType::LeftClick;
        return parseCoordinates(input, command, error);
    }
    if (verb == "right_click") {
        command.type = AgentControlCommandType::RightClick;
        return parseCoordinates(input, command, error);
    }
    if (verb == "key") {
        command.type = AgentControlCommandType::Key;
        std::string key;
        if (!(input >> key) || hasTrailingInput(input)) {
            error = "expected exactly one key name or printable character";
            return false;
        }
        command.keyCode = namedKeyCode(key);
        if (command.keyCode == -1) {
            error = "unknown key";
            return false;
        }
        return true;
    }
    if (verb == "text") {
        command.type = AgentControlCommandType::Text;
        std::getline(input, command.text);
        std::size_t first = command.text.find_first_not_of(" \t");
        if (first == std::string::npos) {
            error = "text cannot be empty";
            return false;
        }
        command.text.erase(0, first);
        if (command.text.size() > 512) {
            error = "text is limited to 512 characters";
            return false;
        }
        for (unsigned char ch : command.text) {
            if (ch < 0x20 || ch > 0x7E) {
                error = "text must contain printable ASCII characters";
                return false;
            }
        }
        return true;
    }
    if (verb == "game_move") {
        command.type = AgentControlCommandType::GameMove;
        std::string gait = "walk";
        if (!(input >> command.tile >> command.elevation)) {
            error = "expected: game_move <tile> <elevation> [walk|run]";
            return false;
        }
        if (input >> gait) {
            if (hasTrailingInput(input)) {
                error = "game_move has too many arguments";
                return false;
            }
        }
        if (command.tile < 0 || command.elevation < 0 || command.elevation > 2
            || (gait != "walk" && gait != "run")) {
            error = "game_move requires a valid tile, elevation 0-2, and walk or run";
            return false;
        }
        command.running = gait == "run";
        return true;
    }
    if (verb == "game_face") {
        command.type = AgentControlCommandType::GameFace;
        if (!(input >> command.rotation) || hasTrailingInput(input)
            || command.rotation < 0 || command.rotation >= 6) {
            error = "game_face requires one rotation from 0 through 5";
            return false;
        }
        return true;
    }
    if (verb == "game_door") {
        command.type = AgentControlCommandType::GameDoor;
        return parseEntityId(input, command, error);
    }
    if (verb == "game_pickup") {
        command.type = AgentControlCommandType::GamePickup;
        return parseEntityId(input, command, error);
    }
    if (verb == "game_loot") {
        command.type = AgentControlCommandType::GameLoot;
        return parseEntityId(input, command, error);
    }
    if (verb == "game_skill") {
        command.type = AgentControlCommandType::GameSkill;
        return parseSkill(input, command, error);
    }
    if (verb == "game_use_item") {
        command.type = AgentControlCommandType::GameUseItem;
        return parseItemUse(input, command, error);
    }
    if (verb == "game_elevator") {
        command.type = AgentControlCommandType::GameElevator;
        return parseElevator(input, command, error);
    }
    if (verb == "game_give") {
        command.type = AgentControlCommandType::GameGive;
        return parseGive(input, command, error);
    }

    error = "unknown command";
    return false;
}

const char* agentControlCommandTypeName(AgentControlCommandType type)
{
    switch (type) {
    case AgentControlCommandType::Move:
        return "move";
    case AgentControlCommandType::LeftClick:
        return "click";
    case AgentControlCommandType::RightClick:
        return "right_click";
    case AgentControlCommandType::Key:
        return "key";
    case AgentControlCommandType::Text:
        return "text";
    case AgentControlCommandType::GameMove:
        return "game_move";
    case AgentControlCommandType::GameFace:
        return "game_face";
    case AgentControlCommandType::GameDoor:
        return "game_door";
    case AgentControlCommandType::GamePickup:
        return "game_pickup";
    case AgentControlCommandType::GameLoot:
        return "game_loot";
    case AgentControlCommandType::GameSkill:
        return "game_skill";
    case AgentControlCommandType::GameUseItem:
        return "game_use_item";
    case AgentControlCommandType::GameElevator:
        return "game_elevator";
    case AgentControlCommandType::GameGive:
        return "game_give";
    }
    return "unknown";
}

} // namespace fallout
