#ifndef FALLOUT_MULTIPLAYER_DEVELOPER_LOCAL_SESSION_H_
#define FALLOUT_MULTIPLAYER_DEVELOPER_LOCAL_SESSION_H_

namespace fallout {
namespace multiplayer {

void developerLocalSessionConfigure(int argc, char** argv);
bool developerLocalSessionIsEnabled();
bool developerLocalSessionEnsureStarted();
void developerLocalSessionPrepareForWorldReset();
void developerLocalSessionStop();

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_DEVELOPER_LOCAL_SESSION_H_ */
