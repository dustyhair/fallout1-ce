#define networkWorldRunEngineAuthoritySmokeTest originalUiAuthoritySmokeTest
#include "multiplayer/network_world.cc"
#undef networkWorldRunEngineAuthoritySmokeTest
namespace fallout {
bool nativePipboyBackgroundControl();
bool nativeInventoryBackgroundControl();
bool nativeSkilldexBackgroundControl();
bool nativeOptionsBackgroundControl();
bool nativeEditorBackgroundControl();
namespace multiplayer {
bool networkWorldRunEngineAuthoritySmokeTest(EngineExecutionProbeCounts& counts)
{
    if(!originalUiAuthoritySmokeTest(counts))return false;
    bool pipboy=nativePipboyBackgroundControl();
    bool inventory=nativeInventoryBackgroundControl();
    if(!pipboy && !inventory)std::fprintf(stderr,"NATIVE_TRANSFER_BOUNDARY_NEGATIVE_CONFIRMED mode=remainder\n");
    bool skilldex = false;
    for (PlayerId id : session.players().playerIds()) {
        if (networkWorldPlayerActor(id) == obj_dude) continue;
        ScopedLocalPlayerBinding binding(session, id);
        ScopedLocalPlayerContext context;
        skilldex = binding && nativeSkilldexBackgroundControl();
        break;
    }
    bool options = nativeOptionsBackgroundControl();
    bool editor = nativeEditorBackgroundControl();
    return pipboy && inventory && skilldex && options && editor;
}
}
}
