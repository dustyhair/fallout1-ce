#define networkWorldRunEngineAuthoritySmokeTest originalUiAuthoritySmokeTest
#include "multiplayer/network_world.cc"
#undef networkWorldRunEngineAuthoritySmokeTest
namespace fallout {
bool nativePipboyBackgroundControl();
bool nativeInventoryBackgroundControl();
namespace multiplayer {
bool networkWorldRunEngineAuthoritySmokeTest(EngineExecutionProbeCounts& counts)
{
    if(!originalUiAuthoritySmokeTest(counts))return false;
    bool pipboy=nativePipboyBackgroundControl();
    bool inventory=nativeInventoryBackgroundControl();
    if(!pipboy && !inventory)std::fprintf(stderr,"NATIVE_TRANSFER_BOUNDARY_NEGATIVE_CONFIRMED mode=remainder\n");
    return pipboy && inventory;
}
}
}
