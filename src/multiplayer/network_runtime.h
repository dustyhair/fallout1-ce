#ifndef FALLOUT_MULTIPLAYER_NETWORK_RUNTIME_H_
#define FALLOUT_MULTIPLAYER_NETWORK_RUNTIME_H_

namespace fallout {
namespace multiplayer {

bool networkRuntimeConfigure(int argc, char** argv);
bool networkRuntimeStart();
void networkRuntimeStop();

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_NETWORK_RUNTIME_H_ */
