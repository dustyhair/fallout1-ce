#ifndef FALLOUT_MULTIPLAYER_LOBBY_SCREEN_H_
#define FALLOUT_MULTIPLAYER_LOBBY_SCREEN_H_

namespace fallout {
namespace multiplayer {

enum class MultiplayerLobbyScreenResult {
    Back,
    StartGame,
};

MultiplayerLobbyScreenResult multiplayerLobbyScreen();

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_LOBBY_SCREEN_H_ */
