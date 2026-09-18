#include "multiplayer/network_bootstrap.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>
#include <variant>

#include "multiplayer/protocol.h"

namespace fallout {
namespace multiplayer {
namespace {

bool parsePort(const std::string& text, std::uint16_t& port)
{
    if (text.empty()
        || !std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        return false;
    }

    char* end = nullptr;
    unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || value == 0 || value > 65535) {
        return false;
    }

    port = static_cast<std::uint16_t>(value);
    return true;
}

bool parseJoinAddress(const std::string& value, std::string& address, std::uint16_t& port)
{
    if (value.empty()) {
        return false;
    }

    port = kDefaultMultiplayerPort;
    if (value.front() == '[') {
        std::size_t closingBracket = value.find(']');
        if (closingBracket == std::string::npos || closingBracket == 1) {
            return false;
        }
        address = value.substr(1, closingBracket - 1);
        if (closingBracket + 1 == value.size()) {
            return true;
        }
        if (value[closingBracket + 1] != ':') {
            return false;
        }
        return parsePort(value.substr(closingBracket + 2), port);
    }

    std::size_t firstColon = value.find(':');
    std::size_t lastColon = value.rfind(':');
    if (firstColon != std::string::npos && firstColon == lastColon) {
        address = value.substr(0, firstColon);
        return !address.empty() && parsePort(value.substr(firstColon + 1), port);
    }

    address = value;
    return !address.empty();
}

bool setMode(NetworkLaunchParseResult& result, NetworkLaunchMode mode)
{
    if (result.options.mode == mode) {
        result.error = NetworkLaunchParseError::DuplicateMode;
        return false;
    }
    if (result.options.mode != NetworkLaunchMode::Disabled) {
        result.error = NetworkLaunchParseError::ConflictingModes;
        return false;
    }
    result.options.mode = mode;
    return true;
}

} // namespace

bool parseNetworkJoinEndpoint(const std::string& value, std::string& address, std::uint16_t& port)
{
    return parseJoinAddress(value, address, port);
}

NetworkLaunchParseResult parseNetworkLaunchOptions(int argc, char* const* argv)
{
    NetworkLaunchParseResult result;
    bool developerMode = false;

    for (int index = 1; index < argc; index++) {
        std::string argument = argv[index] != nullptr ? argv[index] : "";
        if (argument == "--multiplayer-dev") {
            developerMode = true;
            continue;
        }

        constexpr const char* hostPrefix = "--multiplayer-host=";
        if (argument == "--multiplayer-host" || argument.rfind(hostPrefix, 0) == 0) {
            if (!setMode(result, NetworkLaunchMode::Host)) {
                return result;
            }

            std::string portText;
            if (argument.rfind(hostPrefix, 0) == 0) {
                portText = argument.substr(std::char_traits<char>::length(hostPrefix));
                if (!parsePort(portText, result.options.port)) {
                    result.error = NetworkLaunchParseError::InvalidPort;
                    return result;
                }
            } else if (index + 1 < argc && argv[index + 1] != nullptr) {
                std::string candidate = argv[index + 1];
                if (!candidate.empty()
                    && std::all_of(candidate.begin(), candidate.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
                    index++;
                    if (!parsePort(candidate, result.options.port)) {
                        result.error = NetworkLaunchParseError::InvalidPort;
                        return result;
                    }
                }
            }
            continue;
        }

        constexpr const char* joinPrefix = "--multiplayer-join=";
        if (argument == "--multiplayer-join" || argument.rfind(joinPrefix, 0) == 0) {
            if (!setMode(result, NetworkLaunchMode::Join)) {
                return result;
            }

            std::string endpoint;
            if (argument.rfind(joinPrefix, 0) == 0) {
                endpoint = argument.substr(std::char_traits<char>::length(joinPrefix));
            } else if (index + 1 < argc && argv[index + 1] != nullptr) {
                endpoint = argv[++index];
            }
            if (endpoint.empty() || endpoint.rfind("--", 0) == 0) {
                result.error = NetworkLaunchParseError::MissingJoinAddress;
                return result;
            }
            if (!parseJoinAddress(endpoint, result.options.address, result.options.port)) {
                result.error = NetworkLaunchParseError::InvalidPort;
                return result;
            }
        }
    }

    if (developerMode && result.options.mode != NetworkLaunchMode::Disabled) {
        result.error = NetworkLaunchParseError::DevelopmentModeConflict;
    }
    return result;
}

const char* networkLaunchParseErrorMessage(NetworkLaunchParseError error)
{
    switch (error) {
    case NetworkLaunchParseError::None:
        return "no error";
    case NetworkLaunchParseError::DuplicateMode:
        return "the multiplayer launch mode was specified more than once";
    case NetworkLaunchParseError::ConflictingModes:
        return "host and join modes cannot be used together";
    case NetworkLaunchParseError::MissingJoinAddress:
        return "join mode requires a host address";
    case NetworkLaunchParseError::InvalidPort:
        return "the multiplayer port must be between 1 and 65535";
    case NetworkLaunchParseError::DevelopmentModeConflict:
        return "network host/join mode cannot be combined with --multiplayer-dev";
    }
    return "unknown launch error";
}

bool NetworkBootstrap::start(const NetworkLaunchOptions& options,
    std::uint64_t contentDigest,
    SessionId hostSessionId,
    std::uint32_t connectTimeoutMilliseconds)
{
    stop();
    _options = options;
    _error = NetworkBootstrapError::None;
    _rejection = HandshakeRejection::None;
    _contentDigest = contentDigest;
    _sessionId = {};
    _localPlayerId = {};
    _reconnectToken = {};
    _peerIdentity.reset();

    if (options.mode == NetworkLaunchMode::Disabled) {
        _state = NetworkBootstrapState::Disabled;
        return true;
    }
    if (contentDigest == 0
        || (options.mode == NetworkLaunchMode::Join && options.port == 0)) {
        fail(NetworkBootstrapError::InvalidOptions);
        return false;
    }

    if (options.mode == NetworkLaunchMode::Host) {
        if (hostSessionId.value == 0) {
            fail(NetworkBootstrapError::InvalidOptions);
            return false;
        }
        TcpListenResult listening = listenTcp(options.port);
        if (!listening) {
            fail(NetworkBootstrapError::ListenFailed);
            return false;
        }
        _listener = std::move(listening.listener);
        _options.port = _listener->port();
        _sessionId = hostSessionId;
        _localPlayerId = kHostPlayerId;
        if (!generateReconnectToken(_reconnectToken)) {
            fail(NetworkBootstrapError::InvalidOptions);
            return false;
        }
        _state = NetworkBootstrapState::Listening;
        return true;
    }

    if (options.mode != NetworkLaunchMode::Join || options.address.empty()) {
        fail(NetworkBootstrapError::InvalidOptions);
        return false;
    }

    TcpConnectResult connection = connectTcp(options.address, options.port, connectTimeoutMilliseconds);
    if (!connection) {
        fail(NetworkBootstrapError::ConnectFailed);
        return false;
    }
    _transport = std::move(connection.transport);
    _localPlayerId = kGuestPlayerId;
    if (!sendHandshake(HandshakeHello { contentDigest })) {
        return false;
    }
    _state = NetworkBootstrapState::AwaitingHandshake;
    return true;
}

void NetworkBootstrap::poll()
{
    if ((_state == NetworkBootstrapState::Connected || _state == NetworkBootstrapState::Rejected)
        && _transport != nullptr) {
        _transport->poll();
        if (_state == NetworkBootstrapState::Connected && !_transport->isConnected()) {
            fail(NetworkBootstrapError::Disconnected);
        }
        return;
    }

    if (_state == NetworkBootstrapState::Listening) {
        _transport = _listener->accept();
        if (_transport == nullptr) {
            return;
        }
        _state = NetworkBootstrapState::AwaitingHandshake;
    }

    if (_state != NetworkBootstrapState::AwaitingHandshake) {
        return;
    }

    if (_options.mode == NetworkLaunchMode::Host) {
        pollHostHandshake();
    } else {
        pollGuestHandshake();
    }
}

void NetworkBootstrap::stop()
{
    if (_listener != nullptr) {
        _listener->close();
        _listener.reset();
    }
    if (_transport != nullptr) {
        _transport->close();
        _transport.reset();
    }
    _reconnectToken = {};
    _peerIdentity.reset();
    _state = NetworkBootstrapState::Stopped;
}

NetworkBootstrapState NetworkBootstrap::state() const
{
    return _state;
}

NetworkBootstrapError NetworkBootstrap::error() const
{
    return _error;
}

HandshakeRejection NetworkBootstrap::rejection() const
{
    return _rejection;
}

NetworkLaunchMode NetworkBootstrap::mode() const
{
    return _options.mode;
}

std::uint16_t NetworkBootstrap::port() const
{
    return _options.port;
}

SessionId NetworkBootstrap::sessionId() const
{
    return _sessionId;
}

PlayerId NetworkBootstrap::localPlayerId() const
{
    return _localPlayerId;
}

ReconnectToken NetworkBootstrap::reconnectToken() const
{
    return _reconnectToken;
}

std::optional<TransportPeerIdentity> NetworkBootstrap::peerIdentity() const
{
    return _peerIdentity;
}

std::unique_ptr<Transport> NetworkBootstrap::takeTransport()
{
    if (_state != NetworkBootstrapState::Connected) {
        return nullptr;
    }
    return std::move(_transport);
}

std::unique_ptr<Transport> NetworkBootstrap::acceptReconnectTransport()
{
    if (_options.mode != NetworkLaunchMode::Host
        || _state != NetworkBootstrapState::Connected
        || _listener == nullptr
        || !_listener->isOpen()) {
        return nullptr;
    }
    return _listener->accept();
}

bool NetworkBootstrap::sendHandshake(const HandshakeMessage& message)
{
    ProtocolEnvelope envelope;
    envelope.sequence = 1;
    if (encodeHandshakeMessage(message, envelope) != HandshakeError::None) {
        fail(NetworkBootstrapError::EncodeFailed);
        return false;
    }

    Packet packet;
    if (encodeEnvelope(envelope, packet) != ProtocolError::None) {
        fail(NetworkBootstrapError::EncodeFailed);
        return false;
    }
    if (_transport == nullptr || _transport->send(std::move(packet)) != TransportSendResult::Sent) {
        fail(NetworkBootstrapError::SendFailed);
        return false;
    }
    return true;
}

void NetworkBootstrap::pollHostHandshake()
{
    std::optional<Packet> packet = _transport->receive();
    if (!packet.has_value()) {
        if (!_transport->isConnected()) {
            fail(NetworkBootstrapError::Disconnected);
        }
        return;
    }

    ProtocolDecodeResult decodedEnvelope = decodeEnvelope(*packet);
    if (!decodedEnvelope || decodedEnvelope.envelope.sequence != 1) {
        fail(NetworkBootstrapError::ProtocolError);
        return;
    }
    HandshakeDecodeResult decodedHandshake = decodeHandshakeMessage(decodedEnvelope.envelope);
    if (!decodedHandshake) {
        fail(NetworkBootstrapError::HandshakeError);
        return;
    }
    const HandshakeHello* hello = std::get_if<HandshakeHello>(&decodedHandshake.message);
    if (hello == nullptr) {
        fail(NetworkBootstrapError::UnexpectedHandshake);
        return;
    }

    HandshakeMessage response = makeHostHandshakeResponse(*hello, _contentDigest, _sessionId, _reconnectToken);
    const HandshakeRejected* rejected = std::get_if<HandshakeRejected>(&response);
    if (!sendHandshake(response)) {
        return;
    }
    if (rejected != nullptr) {
        _rejection = rejected->reason;
        _state = NetworkBootstrapState::Rejected;
    } else {
        _state = NetworkBootstrapState::Connected;
    }
}

void NetworkBootstrap::pollGuestHandshake()
{
    std::optional<Packet> packet = _transport->receive();
    if (!packet.has_value()) {
        if (!_transport->isConnected()) {
            fail(NetworkBootstrapError::Disconnected);
        }
        return;
    }

    ProtocolDecodeResult decodedEnvelope = decodeEnvelope(*packet);
    if (!decodedEnvelope || decodedEnvelope.envelope.sequence != 1) {
        fail(NetworkBootstrapError::ProtocolError);
        return;
    }
    HandshakeDecodeResult decodedHandshake = decodeHandshakeMessage(decodedEnvelope.envelope);
    if (!decodedHandshake) {
        fail(NetworkBootstrapError::HandshakeError);
        return;
    }

    if (const auto* welcome = std::get_if<HandshakeWelcome>(&decodedHandshake.message)) {
        if (welcome->assignedPlayerId != kGuestPlayerId) {
            fail(NetworkBootstrapError::UnexpectedHandshake);
            return;
        }
        _sessionId = welcome->sessionId;
        _reconnectToken = welcome->reconnectToken;
        _peerIdentity = _transport->peerIdentity();
        if (!_peerIdentity.has_value()) {
            fail(NetworkBootstrapError::HandshakeError);
            return;
        }
        _state = NetworkBootstrapState::Connected;
        return;
    }
    if (const auto* rejected = std::get_if<HandshakeRejected>(&decodedHandshake.message)) {
        _rejection = rejected->reason;
        _state = NetworkBootstrapState::Rejected;
        return;
    }
    fail(NetworkBootstrapError::UnexpectedHandshake);
}

void NetworkBootstrap::fail(NetworkBootstrapError error)
{
    _error = error;
    _state = NetworkBootstrapState::Failed;
}

const char* networkBootstrapErrorMessage(NetworkBootstrapError error)
{
    switch (error) {
    case NetworkBootstrapError::None:
        return "no error";
    case NetworkBootstrapError::InvalidOptions:
        return "invalid network launch options";
    case NetworkBootstrapError::ListenFailed:
        return "could not listen on the requested TCP port";
    case NetworkBootstrapError::ConnectFailed:
        return "could not connect to the multiplayer host";
    case NetworkBootstrapError::EncodeFailed:
        return "could not encode the connection handshake";
    case NetworkBootstrapError::SendFailed:
        return "could not send the connection handshake";
    case NetworkBootstrapError::ProtocolError:
        return "received an invalid protocol packet";
    case NetworkBootstrapError::HandshakeError:
        return "received an invalid connection handshake";
    case NetworkBootstrapError::UnexpectedHandshake:
        return "received an unexpected connection handshake";
    case NetworkBootstrapError::Disconnected:
        return "the multiplayer peer disconnected";
    }
    return "unknown connection error";
}

const char* handshakeRejectionMessage(HandshakeRejection rejection)
{
    switch (rejection) {
    case HandshakeRejection::None:
        return "no rejection";
    case HandshakeRejection::ContentMismatch:
        return "the host and guest game data do not match";
    case HandshakeRejection::SessionFull:
        return "the multiplayer session is full";
    case HandshakeRejection::ServerUnavailable:
        return "the multiplayer host is unavailable";
    case HandshakeRejection::InvalidReconnect:
        return "the multiplayer reconnect credential was rejected";
    }
    return "unknown rejection";
}

} // namespace multiplayer
} // namespace fallout
