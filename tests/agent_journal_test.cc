#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "agent_journal.h"

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        return 1;
    }

    std::string option = "--agent-journal=" + std::string(argv[1]);
    char program[] = "agent-journal-test";
    std::vector<char> optionBuffer(option.begin(), option.end());
    optionBuffer.push_back('\0');
    char* arguments[] = { program, optionBuffer.data() };

    bool passed = expect(fallout::agentJournalConfigure(2, arguments), "journal accepts an output path")
        && expect(fallout::agentJournalEnabled(), "journal reports that it is enabled");
    fallout::agentJournalWriteText("display", "Vault \"door\"\nopened");
    fallout::agentJournalWriteChat("incoming", 1, "Albert", "Follow me.");

    fallout::AgentJournalWorldState state;
    state.map = "VAULT13.MAP";
    state.phase = "exploration";
    state.connected = true;
    state.host.playerId = 1;
    state.host.name = "Albert";
    state.host.local = true;
    state.host.tile = 12345;
    state.guest.playerId = 2;
    state.guest.name = "Max";
    state.guest.tile = 12346;
    fallout::AgentJournalCritterState enemy;
    enemy.entityId = 44;
    enemy.name = "Radscorpion";
    enemy.disposition = "hostile";
    enemy.tile = 12347;
    enemy.screenX = 410;
    enemy.screenY = 220;
    state.visibleCritters.push_back(enemy);
    fallout::agentJournalWriteWorldState(state);
    fallout::agentJournalWriteWorldState(state);
    state.guest.rotation = 2;
    fallout::agentJournalWriteWorldState(state);
    fallout::agentJournalWriteWorldExit();
    fallout::agentJournalClose();

    std::ifstream input(argv[1]);
    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);) {
        lines.push_back(std::move(line));
    }
    passed = expect(lines.size() == 7, "journal suppresses duplicate world states") && passed;
    passed = expect(lines.size() > 1
            && lines[1].find("Vault \\\"door\\\"\\nopened") != std::string::npos,
        "journal JSON-escapes display text")
        && passed;
    passed = expect(lines.size() > 2
            && lines[2].find("\"event\":\"chat\"") != std::string::npos
            && lines[2].find("\"direction\":\"incoming\"") != std::string::npos,
        "journal records structured chat direction")
        && passed;
    passed = expect(lines.size() > 3
            && lines[3].find("\"event\":\"world_state\"") != std::string::npos
            && lines[3].find("\"tile\":12345") != std::string::npos
            && lines[3].find("\"visible_critters\":[{\"entity_id\":44") != std::string::npos
            && lines[3].find("\"disposition\":\"hostile\"") != std::string::npos,
        "journal records structured actor and enemy state")
        && passed;
    passed = expect(lines.size() > 6 && lines[6].find("\"event\":\"session_end\"") != std::string::npos,
        "journal closes with a session boundary")
        && passed;
    return passed ? 0 : 1;
}
