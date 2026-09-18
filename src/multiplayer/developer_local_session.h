#ifndef FALLOUT_MULTIPLAYER_DEVELOPER_LOCAL_SESSION_H_
#define FALLOUT_MULTIPLAYER_DEVELOPER_LOCAL_SESSION_H_

#include "multiplayer/types.h"

namespace fallout {

struct Object;

namespace multiplayer {

void developerLocalSessionConfigure(int argc, char** argv);
bool developerLocalSessionIsEnabled();
bool developerLocalSessionEnsureStarted();
bool developerLocalSessionSubmitMove(PlayerId playerId, int destinationTile, int elevation, bool running);
bool developerLocalSessionSubmitDoorUse(PlayerId playerId, Object* target);
void developerLocalSessionPrepareForWorldReset();
void developerLocalSessionStop();

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_DEVELOPER_LOCAL_SESSION_H_ */
