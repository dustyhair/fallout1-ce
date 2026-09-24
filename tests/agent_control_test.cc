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
    passed = expect(parse("46 game_move 12345 1 run", command)
            && command.type == fallout::AgentControlCommandType::GameMove
            && command.tile == 12345
            && command.elevation == 1
            && command.running,
        "parses a semantic movement command")
        && passed;
    passed = expect(parse("47 game_face 5", command)
            && command.type == fallout::AgentControlCommandType::GameFace
            && command.rotation == 5,
        "parses a semantic facing command")
        && passed;
    passed = expect(parse("47 game_attack 82 0 8", command)
            && command.type == fallout::AgentControlCommandType::GameAttack
            && command.entityId == 82 && command.hitMode == 0
            && command.hitLocation == 8,
        "parses a semantic combat attack")
        && passed;
    passed = expect(!parse("47 game_attack 82 6", command)
            && !parse("47 game_attack 82 0 bogus", command),
        "rejects reload mode and malformed attack location")
        && passed;
    passed = expect(parse("47 game_reload 90 right", command)
            && command.type == fallout::AgentControlCommandType::GameReload
            && command.hitMode == 7,
        "parses a semantic combat reload")
        && passed;
    passed = expect(parse("47 game_combat_item 91", command)
            && command.type == fallout::AgentControlCommandType::GameCombatItem
            && command.entityId == 91 && command.destinationEntityId == 0,
        "parses a semantic combat item use")
        && passed;
    passed = expect(parse("47 game_combat_item 91 82", command)
            && command.entityId == 91 && command.destinationEntityId == 82,
        "parses a targeted combat item use")
        && passed;
    passed = expect(parse("47 game_end_turn", command)
            && command.type == fallout::AgentControlCommandType::GameEndTurn,
        "parses a semantic combat end turn")
        && passed;
    passed = expect(parse("48 game_pickup 77", command)
            && command.type == fallout::AgentControlCommandType::GamePickup
            && command.entityId == 77,
        "parses a semantic entity command")
        && passed;
    passed = expect(parse("49 game_give 1 77 12", command)
            && command.type == fallout::AgentControlCommandType::GameGive
            && command.destinationEntityId == 1
            && command.entityId == 77
            && command.quantity == 12,
        "parses a semantic player gift")
        && passed;
    passed = expect(parse("50 game_skill traps 77", command)
            && command.type == fallout::AgentControlCommandType::GameSkill
            && command.skill == 11
            && command.entityId == 77,
        "parses a semantic targeted skill command")
        && passed;
    passed = expect(parse("51 game_use_item 81 77", command)
            && command.type == fallout::AgentControlCommandType::GameUseItem
            && command.itemEntityId == 81
            && command.entityId == 77,
        "parses a semantic item-on-target command")
        && passed;
    passed = expect(parse("52 game_elevator 8 2", command)
            && command.type == fallout::AgentControlCommandType::GameElevator
            && command.elevatorType == 8
            && command.elevatorLevel == 1,
        "parses an authoritative elevator type and one-based level")
        && passed;
    passed = expect(parse("53 game_exit 82", command)
            && command.type == fallout::AgentControlCommandType::GameExit
            && command.entityId == 82,
        "parses an authoritative exit-grid entity")
        && passed;
    passed = expect(parse("54 game_stairs 83", command)
            && command.type == fallout::AgentControlCommandType::GameStairs
            && command.entityId == 83,
        "parses an authoritative stairs or ladder entity")
        && passed;
    passed = expect(parse("55 game_rest 180", command)
            && command.type == fallout::AgentControlCommandType::GameRest
            && command.restMinutes == 180,
        "parses a fixed-duration rest proposal or approval")
        && passed;
    passed = expect(parse("56 game_rest cancel", command)
            && command.type == fallout::AgentControlCommandType::GameRest
            && command.restMinutes == 0,
        "parses rest consent withdrawal")
        && passed;
    passed = expect(parse("56 game_rest until_morning", command)
            && command.restMinutes == -1,
        "parses next-morning rest choice") && passed;
    passed = expect(parse("56 game_rest until_healed", command)
            && command.restMinutes == -5,
        "parses until-healed rest choice") && passed;
    passed = expect(!parse("0 click 10 10", command), "rejects command id zero") && passed;
    passed = expect(!parse("52 click -1 10", command), "rejects negative coordinates") && passed;
    passed = expect(!parse("53 key definitely-not-a-key", command), "rejects an unknown key") && passed;
    passed = expect(!parse("54 move 10 20 trailing", command), "rejects trailing coordinate input") && passed;
    passed = expect(!parse("55 game_move 10 3 walk", command), "rejects an invalid elevation") && passed;
    passed = expect(!parse("56 game_door 0", command), "rejects an invalid entity id") && passed;
    passed = expect(!parse("57 game_give 1 77 0", command), "rejects a zero gift quantity") && passed;
    passed = expect(!parse("58 game_skill gambling 77", command), "rejects an unsupported targeted skill") && passed;
    passed = expect(!parse("59 game_use_item 77 77", command), "rejects using an item on itself") && passed;
    passed = expect(!parse("60 game_elevator 12 1", command), "rejects an unknown elevator type") && passed;
    passed = expect(!parse("61 game_elevator 8 5", command), "rejects an unavailable elevator level") && passed;
    passed = expect(!parse("62 game_exit 0", command), "rejects an invalid exit-grid entity") && passed;
    passed = expect(!parse("63 game_stairs 0", command), "rejects an invalid stairs or ladder entity") && passed;
    passed = expect(!parse("64 game_rest 17", command), "rejects an unsupported rest duration") && passed;

    return passed ? 0 : 1;
}
