#define networkWorldRunEngineAuthoritySmokeTest originalPausedHudAuthoritySmokeTest
#include "multiplayer/network_world.cc"
#undef networkWorldRunEngineAuthoritySmokeTest
namespace fallout {
bool nativePausedHudControl();
namespace multiplayer {
bool networkWorldRunEngineAuthoritySmokeTest(EngineExecutionProbeCounts& counts)
{
    return originalPausedHudAuthoritySmokeTest(counts) && nativePausedHudControl();
}
}
}
