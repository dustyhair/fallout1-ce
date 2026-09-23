#include "game/engine_execution_probe.h"

namespace fallout {
namespace {

bool active = false;
EngineExecutionProbeCounts counts;

} // namespace

void engineExecutionProbeBegin()
{
    counts = {};
    active = true;
}

EngineExecutionProbeCounts engineExecutionProbeEnd()
{
    active = false;
    return counts;
}

void engineExecutionProbeRecordScriptProcedure()
{
    if (active) {
        counts.scriptProcedures++;
    }
}

void engineExecutionProbeRecordCombatAttack()
{
    if (active) {
        counts.combatAttacks++;
    }
}

void engineExecutionProbeRecordRandomDraw()
{
    if (active) {
        counts.randomDraws++;
    }
}

} // namespace fallout
