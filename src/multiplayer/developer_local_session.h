#ifndef FALLOUT_MULTIPLAYER_DEVELOPER_LOCAL_SESSION_H_
#define FALLOUT_MULTIPLAYER_DEVELOPER_LOCAL_SESSION_H_

#include <cstdint>

#include "multiplayer/save_sidecar.h"
#include "multiplayer/types.h"

namespace fallout {

struct Object;

namespace multiplayer {

void developerLocalSessionConfigure(int argc, char** argv);
bool developerLocalSessionIsEnabled();
bool developerLocalSessionIsActive();
bool developerLocalSessionEnsureStarted();
MultiplayerSaveError developerLocalSessionCaptureSave(std::uint64_t generation,
    std::uint64_t saveDatDigest,
    MultiplayerSaveSidecar& sidecar);
bool developerLocalSessionStageLoadedSave(const MultiplayerSaveSidecar& sidecar);
bool developerLocalSessionWriteGuestObject(const char* relativePath);
bool developerLocalSessionStageLoadedGuestObject(const char* relativePath);
void developerLocalSessionRejectLoadedSave();
bool developerLocalSessionOpenGuestInventory();
bool developerLocalSessionSubmitMove(PlayerId playerId, int destinationTile, int elevation, bool running);
bool developerLocalSessionSubmitDoorUse(PlayerId playerId, Object* target);
bool developerLocalSessionSubmitPickup(PlayerId playerId, Object* target);
bool developerLocalSessionSubmitLoot(PlayerId playerId, Object* target);
void developerLocalSessionPrepareForWorldReset();
void developerLocalSessionStop();

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_DEVELOPER_LOCAL_SESSION_H_ */
