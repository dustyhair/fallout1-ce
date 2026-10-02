// Exercise the real native combat-script request dispatcher without opening a
// modal elevator window. The actual capacity fixture tests the runtime stop;
// this probe selects that stopped state and observes dispatcher entry behavior.
static bool simulationStopped = false;
static int elevatorCalls = 0;
#define networkRuntimeSimulationStopped testSimulationStopped
#define elevator_select testElevatorSelect
#include "game/scripts.cc"
#undef elevator_select
#undef networkRuntimeSimulationStopped

#include <cstdio>

namespace fallout {
bool GNW95_isActive = false;
char GNW95_title[256];
int testElevatorSelect(int, int*, int*, int*)
{
    ++elevatorCalls;
    return -1;
}
namespace multiplayer {
bool testSimulationStopped()
{
    return simulationStopped;
}
}
}

int main()
{
    using namespace fallout;
    scriptState.requests = SCRIPT_REQUEST_ELEVATOR;
    scriptState.elevatorType = 0;
    simulationStopped = true;
    bool stopped = scripts_check_state_in_combat() == 0
        && elevatorCalls == 0 && scriptState.requests == 0;
    // The same request must reach the native selector in an ordinary session.
    simulationStopped = false;
    elevatorCalls = 0;
    scriptState.requests = SCRIPT_REQUEST_ELEVATOR;
    bool resumed = scripts_check_state_in_combat() == 0
        && elevatorCalls == 1 && scriptState.requests == 0;
    std::printf("NATIVE_STOPPED_SCRIPT_ENTRY stopped=%d resumed=%d elevator_calls=%d\n",
        stopped, resumed, elevatorCalls);
    return stopped && resumed ? 0 : 1;
}
