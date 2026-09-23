#ifndef FALLOUT_MULTIPLAYER_NETWORK_BOOTSTRAP_H_
#define FALLOUT_MULTIPLAYER_NETWORK_BOOTSTRAP_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "multiplayer/connection_handshake.h"
#include "multiplayer/tcp_transport.h"
#include "multiplayer/transport.h"
#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

constexpr std::uint16_t kDefaultMultiplayerPort = 42424;

enum class NetworkLaunchMode {
    Disabled,
    Host,
    Join,
};

struct NetworkLaunchOptions {
    NetworkLaunchMode mode = NetworkLaunchMode::Disabled;
    std::string address;
    std::uint16_t port = kDefaultMultiplayerPort;
    std::optional<TransportPeerIdentity> expectedHostIdentity;
};

enum class NetworkLaunchParseError {
    None,
    DuplicateMode,
    ConflictingModes,
    MissingJoinAddress,
    InvalidPort,
    InvalidHostIdentity,
    DuplicateHostIdentity,
    HostIdentityWithoutJoin,
    DevelopmentModeConflict,
};

struct NetworkLaunchParseResult {
    NetworkLaunchParseError error = NetworkLaunchParseError::None;
    NetworkLaunchOptions options;

    explicit operator bool() const
    {
        return error == NetworkLaunchParseError::None;
    }
};

NetworkLaunchParseResult parseNetworkLaunchOptions(int argc, char* const* argv);
bool parseNetworkJoinEndpoint(const std::string& value, std::string& address, std::uint16_t& port);
bool parseTransportPeerIdentity(const std::string& value, TransportPeerIdentity& identity);
std::string formatTransportPeerIdentity(const TransportPeerIdentity& identity);
const char* networkLaunchParseErrorMessage(NetworkLaunchParseError error);

enum class NetworkBootstrapState {
    Disabled,
    Listening,
    AwaitingHandshake,
    Connected,
    Rejected,
    Failed,
    Stopped,
};

enum class NetworkBootstrapError {
    None,
    InvalidOptions,
    ListenFailed,
    ConnectFailed,
    EncodeFailed,
    SendFailed,
    ProtocolError,
    HandshakeError,
    UnexpectedHandshake,
    HostIdentityMismatch,
    Disconnected,
};

class NetworkBootstrap {
public:
    bool start(const NetworkLaunchOptions& options,
        std::uint64_t contentDigest,
        SessionId hostSessionId = {},
        std::uint32_t connectTimeoutMilliseconds = 5000);
    void poll();
    void stop();

    NetworkBootstrapState state() const;
    NetworkBootstrapError error() const;
    HandshakeRejection rejection() const;
    NetworkLaunchMode mode() const;
    std::uint16_t port() const;
    SessionId sessionId() const;
    PlayerId localPlayerId() const;
    ReconnectToken reconnectToken() const;
    std::optional<TransportPeerIdentity> localIdentity() const;
    std::optional<TransportPeerIdentity> peerIdentity() const;
    std::unique_ptr<Transport> takeTransport();
    std::unique_ptr<Transport> acceptReconnectTransport();

private:
    bool sendHandshake(const HandshakeMessage& message);
    void pollHostHandshake();
    void pollGuestHandshake();
    void fail(NetworkBootstrapError error);

    NetworkLaunchOptions _options;
    NetworkBootstrapState _state = NetworkBootstrapState::Disabled;
    NetworkBootstrapError _error = NetworkBootstrapError::None;
    HandshakeRejection _rejection = HandshakeRejection::None;
    std::uint64_t _contentDigest = 0;
    SessionId _sessionId;
    PlayerId _localPlayerId;
    ReconnectToken _reconnectToken;
    std::optional<TransportPeerIdentity> _localIdentity;
    std::optional<TransportPeerIdentity> _peerIdentity;
    std::unique_ptr<TcpListener> _listener;
    std::unique_ptr<Transport> _transport;
};

const char* networkBootstrapErrorMessage(NetworkBootstrapError error);
const char* handshakeRejectionMessage(HandshakeRejection rejection);

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_NETWORK_BOOTSTRAP_H_ */
