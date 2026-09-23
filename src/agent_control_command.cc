#include "agent_control_command.h"

#include <algorithm>
#include <cctype>
#include <sstream>

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
    }
    return "unknown";
}

} // namespace fallout
