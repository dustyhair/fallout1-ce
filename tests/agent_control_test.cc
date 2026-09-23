#include <iostream>
#include <string>

#include "agent_control_command.h"
#include "plib/gnw/kb.h"

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

bool parse(const char* line, fallout::AgentControlCommand& command)
{
    std::string error;
    return fallout::agentControlParseCommand(line, command, error);
}

} // namespace

int main()
{
    bool passed = true;
    fallout::AgentControlCommand command;

    passed = expect(parse("41 click 410 220", command)
            && command.id == 41
            && command.type == fallout::AgentControlCommandType::LeftClick
            && command.x == 410
            && command.y == 220,
        "parses a targeted click")
        && passed;
    passed = expect(parse("42 right_click 12 34", command)
            && command.type == fallout::AgentControlCommandType::RightClick,
        "parses a right click")
        && passed;
    passed = expect(parse("43 key enter", command) && command.keyCode == fallout::KEY_RETURN,
        "parses a named key")
        && passed;
    passed = expect(parse("44 key a", command) && command.keyCode == fallout::KEY_LOWERCASE_A,
        "parses a printable key")
        && passed;
    passed = expect(parse("45 text Follow me.", command) && command.text == "Follow me.",
        "preserves command text")
        && passed;
    passed = expect(!parse("0 click 10 10", command), "rejects command id zero") && passed;
    passed = expect(!parse("46 click -1 10", command), "rejects negative coordinates") && passed;
    passed = expect(!parse("47 key definitely-not-a-key", command), "rejects an unknown key") && passed;
    passed = expect(!parse("48 move 10 20 trailing", command), "rejects trailing coordinate input") && passed;

    return passed ? 0 : 1;
}
