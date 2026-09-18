#ifndef FALLOUT_MULTIPLAYER_NETWORK_LOBBY_H_
#define FALLOUT_MULTIPLAYER_NETWORK_LOBBY_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>

#include "multiplayer/character_lobby.h"
#include "multiplayer/network_bootstrap.h"
#include "multiplayer/transport.h"
#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

constexpr std::uint16_t kNetworkLobbyVersion = 1;

enum class NetworkLobbyState {
    Disabled,
    Waiting,
    Ready,
    Rejected,
    Failed,
    Stopped,
};

enum class NetworkLobbyError {
    None,
    InvalidStart,
    EncodeFailed,
    SendFailed,
    ProtocolError,
    UnexpectedMessage,
    PeerSheetRejected,
    Disconnected,
};

class NetworkLobby {
public:
    bool start(NetworkLaunchMode mode, SessionId sessionId, std::unique_ptr<Transport> transport);
    CharacterLobbyError submitLocalSheet(const CharacterCreationSheet& sheet);
    bool requestStart();
    bool sendLocalMove(int destinationTile, int elevation, bool running);
    std::optional<ActorMovementStartedEvent> takePeerMove();
    void poll();
    void stop();

    NetworkLobbyState state() const;
    NetworkLobbyError error() const;
    CharacterLobbyError sheetError() const;
    const CharacterCreationSheet* localSheet() const;
    const CharacterCreationSheet* peerSheet() const;
    bool startRequested() const;
    std::uint64_t nextSendSequence() const;
    std::uint64_t nextReceiveSequence() const;
    std::unique_ptr<Transport> takeTransport();

private:
    enum class MessageType : std::uint8_t {
        CharacterSheet = 1,
        Ready = 2,
        Rejected = 3,
        Start = 4,
    };

    bool sendMessage(MessageType type, const std::vector<std::uint8_t>& body = {});
    void handlePacket(const Packet& packet);
    void handleCharacterSheet(const std::vector<std::uint8_t>& body);
    void tryReadyHost();
    void reject(CharacterLobbyError error);
    void fail(NetworkLobbyError error);

    NetworkLaunchMode _mode = NetworkLaunchMode::Disabled;
    NetworkLobbyState _state = NetworkLobbyState::Disabled;
    NetworkLobbyError _error = NetworkLobbyError::None;
    CharacterLobbyError _sheetError = CharacterLobbyError::None;
    SessionId _sessionId;
    std::uint64_t _nextSendSequence = 2;
    std::uint64_t _nextReceiveSequence = 2;
    std::optional<CharacterCreationSheet> _localSheet;
    std::optional<CharacterCreationSheet> _peerSheet;
    bool _startRequested = false;
    std::uint64_t _nextMovementSequence = 1;
    std::deque<ActorMovementStartedEvent> _peerMoves;
    std::unique_ptr<Transport> _transport;
};

const char* networkLobbyErrorMessage(NetworkLobbyError error);
const char* characterLobbyErrorMessage(CharacterLobbyError error);

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_NETWORK_LOBBY_H_ */
