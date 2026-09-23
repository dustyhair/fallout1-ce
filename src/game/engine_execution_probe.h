#ifndef FALLOUT_GAME_ENGINE_EXECUTION_PROBE_H_
#define FALLOUT_GAME_ENGINE_EXECUTION_PROBE_H_

#include <cstdint>

namespace fallout {

struct EngineExecutionProbeCounts {
    std::uint32_t scriptProcedures = 0;
    std::uint32_t combatAttacks = 0;
    std::uint32_t randomDraws = 0;
};

void engineExecutionProbeBegin();
EngineExecutionProbeCounts engineExecutionProbeEnd();
void engineExecutionProbeRecordScriptProcedure();
void engineExecutionProbeRecordCombatAttack();
void engineExecutionProbeRecordRandomDraw();

} // namespace fallout

#endif /* FALLOUT_GAME_ENGINE_EXECUTION_PROBE_H_ */
