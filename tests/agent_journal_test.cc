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
    fallout::agentJournalWriteAgentCommand(17, "click", "executed", "click injected");

    fallout::AgentJournalWorldState state;
    state.map = "VAULT13.MAP";
    state.phase = "exploration";
    state.connected = true;
    state.host.playerId = 1;
    state.host.entityId = 1;
    state.host.name = "Albert";
    state.host.local = true;
    state.host.tile = 12345;
    state.guest.playerId = 2;
    state.guest.entityId = 2;
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
    fallout::AgentJournalInventoryItemState item;
    item.entityId = 66;
    item.pid = 41;
    item.name = "Bottle caps";
    item.quantity = 12;
    state.localInventory.push_back(item);
    fallout::AgentJournalInteractableState door;
    door.entityId = 77;
    door.kind = "door";
    door.name = "Vault door";
    door.tile = 12348;
    door.screenX = 440;
    door.screenY = 220;
    door.distance = 2;
    door.locked = true;
    state.visibleInteractables.push_back(door);
    fallout::AgentJournalWorldState::Trade trade;
    trade.id = 9;
    trade.revision = 3;
    fallout::AgentJournalWorldState::Trade::Participant trader;
    trader.playerId = 1;
    trader.actorId = 1;
    trader.caps = 12;
    trader.confirmed = true;
    trader.items.push_back({ 66, 2 });
    trade.participants.push_back(trader);
    state.trade = trade;
    fallout::agentJournalWriteWorldState(state);
    fallout::agentJournalWriteWorldState(state);
    state.pendingRestMinutes = 30;
    state.pendingRestProposerId = 2;
    state.pendingRestProposerName = "Max";
    fallout::agentJournalWriteWorldState(state);
    state.guest.rotation = 2;
    state.pendingRestMinutes = -5;
    fallout::agentJournalWriteWorldState(state);
    fallout::agentJournalWriteWorldExit();
    fallout::agentJournalClose();

    std::ifstream input(argv[1]);
    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);) {
        lines.push_back(std::move(line));
    }
    passed = expect(lines.size() == 9, "journal suppresses duplicate world states") && passed;
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
            && lines[3].find("\"event\":\"agent_command\"") != std::string::npos
            && lines[3].find("\"command_id\":17") != std::string::npos
            && lines[3].find("\"status\":\"executed\"") != std::string::npos,
        "journal records agent command acknowledgements")
        && passed;
    passed = expect(lines.size() > 4
            && lines[4].find("\"event\":\"world_state\"") != std::string::npos
            && lines[4].find("\"tile\":12345") != std::string::npos
            && lines[4].find("\"local_inventory\":[{\"entity_id\":66") != std::string::npos
            && lines[4].find("\"quantity\":12") != std::string::npos
            && lines[4].find("\"visible_critters\":[{\"entity_id\":44") != std::string::npos
            && lines[4].find("\"disposition\":\"hostile\"") != std::string::npos
            && lines[4].find("\"visible_interactables\":[{\"entity_id\":77") != std::string::npos
            && lines[4].find("\"kind\":\"door\"") != std::string::npos
            && lines[4].find("\"locked\":true") != std::string::npos,
        "journal records structured actors, enemies, and interactable targets")
        && passed;
    passed = expect(lines.size() > 4
            && lines[4].find("\"trade\":{\"id\":9,\"revision\":3")
                != std::string::npos
            && lines[4].find("\"confirmed\":true") != std::string::npos,
        "journal exposes the active trade revision and offer") && passed;
    passed = expect(lines.size() > 5
            && lines[5].find("\"pending_rest\":{\"minutes\":30") != std::string::npos
            && lines[5].find("\"choice\":\"fixed\"") != std::string::npos
            && lines[5].find("\"proposer_player_id\":2") != std::string::npos
            && lines[5].find("\"proposer_name\":\"Max\"") != std::string::npos,
        "journal attributes a pending rest proposal to its player")
        && passed;
    passed = expect(lines.size() > 6
            && lines[6].find("\"minutes\":-5,\"choice\":\"until_healed\"") != std::string::npos,
        "journal names an until-healed proposal") && passed;
    passed = expect(lines.size() > 8 && lines[8].find("\"event\":\"session_end\"") != std::string::npos,
        "journal closes with a session boundary")
        && passed;
    return passed ? 0 : 1;
}
