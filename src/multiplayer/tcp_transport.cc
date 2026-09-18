#include "multiplayer/tcp_transport.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fallout {
namespace multiplayer {
namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

constexpr std::size_t kTlsCertificateBufferSize = 4096;

bool initializeSocketApi()
{
#if defined(_WIN32)
    static const bool initialized = []() {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
#else
    return true;
#endif
}

void closeSocket(SocketHandle socket)
{
    if (socket == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
}

int lastSocketError()
{
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool isWouldBlock(int error)
{
#if defined(_WIN32)
    return error == WSAEWOULDBLOCK;
#else
    return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

bool isConnectInProgress(int error)
{
#if defined(_WIN32)
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINVAL;
#else
    return error == EINPROGRESS || error == EALREADY || isWouldBlock(error);
#endif
}

bool setNonBlocking(SocketHandle socket)
{
#if defined(_WIN32)
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    int flags = fcntl(socket, F_GETFL, 0);
    return flags != -1 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
}

bool waitForConnect(SocketHandle socket, std::uint32_t timeoutMilliseconds)
{
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(socket, &writable);
    timeval timeout;
    timeout.tv_sec = static_cast<long>(timeoutMilliseconds / 1000);
    timeout.tv_usec = static_cast<long>((timeoutMilliseconds % 1000) * 1000);
#if defined(_WIN32)
    int ready = select(0, nullptr, &writable, nullptr, &timeout);
#else
    int ready = select(socket + 1, nullptr, &writable, nullptr, &timeout);
#endif
    if (ready <= 0 || !FD_ISSET(socket, &writable)) {
        return false;
    }

    int socketError = 0;
#if defined(_WIN32)
    int socketErrorSize = sizeof(socketError);
#else
    socklen_t socketErrorSize = sizeof(socketError);
#endif
    return getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &socketErrorSize) == 0
        && socketError == 0;
}

void setNoDelay(SocketHandle socket)
{
    int enabled = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
}

std::ptrdiff_t sendBytes(SocketHandle socket, const std::uint8_t* bytes, std::size_t size)
{
    int chunkSize = static_cast<int>(std::min<std::size_t>(size, INT_MAX));
#if defined(_WIN32)
    return send(socket, reinterpret_cast<const char*>(bytes), chunkSize, 0);
#else
    int flags = 0;
#if defined(MSG_NOSIGNAL)
    flags = MSG_NOSIGNAL;
#endif
    return send(socket, bytes, static_cast<std::size_t>(chunkSize), flags);
#endif
}

std::ptrdiff_t receiveBytes(SocketHandle socket, std::uint8_t* bytes, std::size_t size)
{
    int chunkSize = static_cast<int>(std::min<std::size_t>(size, INT_MAX));
#if defined(_WIN32)
    return recv(socket, reinterpret_cast<char*>(bytes), chunkSize, 0);
#else
    return recv(socket, bytes, static_cast<std::size_t>(chunkSize), 0);
#endif
}

void appendFrameSize(std::vector<std::uint8_t>& bytes, std::size_t size)
{
    std::uint32_t value = static_cast<std::uint32_t>(size);
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

std::uint32_t readFrameSize(const std::vector<std::uint8_t>& bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24)
        | (static_cast<std::uint32_t>(bytes[1]) << 16)
        | (static_cast<std::uint32_t>(bytes[2]) << 8)
        | static_cast<std::uint32_t>(bytes[3]);
}

bool identitiesEqual(const TransportPeerIdentity& lhs, const TransportPeerIdentity& rhs)
{
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < lhs.size(); index++) {
        difference |= lhs[index] ^ rhs[index];
    }
    return difference == 0;
}

struct TlsServerIdentity {
    TlsServerIdentity()
    {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&random);
        mbedtls_pk_init(&key);
        mbedtls_x509_crt_init(&certificate);
        mbedtls_ssl_config_init(&config);
    }

    ~TlsServerIdentity()
    {
        mbedtls_ssl_config_free(&config);
        mbedtls_x509_crt_free(&certificate);
        mbedtls_pk_free(&key);
        mbedtls_ctr_drbg_free(&random);
        mbedtls_entropy_free(&entropy);
    }

    bool initialize()
    {
        static constexpr unsigned char personalization[] = "fallout-ce multiplayer host TLS";
        if (mbedtls_ctr_drbg_seed(&random, mbedtls_entropy_func, &entropy,
                personalization, sizeof(personalization) - 1)
            != 0) {
            return false;
        }
        if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0
            || mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                   mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &random)
                != 0) {
            return false;
        }

        std::array<unsigned char, 16> serial;
        if (mbedtls_ctr_drbg_random(&random, serial.data(), serial.size()) != 0) {
            return false;
        }
        serial[0] &= 0x7F;
        serial[0] |= 1;

        mbedtls_x509write_cert writer;
        mbedtls_x509write_crt_init(&writer);
        mbedtls_x509write_crt_set_version(&writer, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&writer, &key);
        mbedtls_x509write_crt_set_issuer_key(&writer, &key);
        int result = mbedtls_x509write_crt_set_subject_name(&writer, "CN=Fallout CE Multiplayer");
        if (result == 0) {
            result = mbedtls_x509write_crt_set_issuer_name(&writer, "CN=Fallout CE Multiplayer");
        }
        if (result == 0) {
            result = mbedtls_x509write_crt_set_serial_raw(&writer, serial.data(), serial.size());
        }
        if (result == 0) {
            result = mbedtls_x509write_crt_set_validity(&writer, "20240101000000", "20491231235959");
        }
        if (result == 0) {
            result = mbedtls_x509write_crt_set_basic_constraints(&writer, 0, -1);
        }
        if (result == 0) {
            result = mbedtls_x509write_crt_set_key_usage(&writer,
                MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_AGREEMENT);
        }
        if (result != 0) {
            mbedtls_x509write_crt_free(&writer);
            return false;
        }

        std::array<unsigned char, kTlsCertificateBufferSize> encoded;
        result = mbedtls_x509write_crt_der(&writer, encoded.data(), encoded.size(),
            mbedtls_ctr_drbg_random, &random);
        mbedtls_x509write_crt_free(&writer);
        if (result <= 0) {
            return false;
        }
        std::size_t encodedSize = static_cast<std::size_t>(result);
        const unsigned char* encodedStart = encoded.data() + encoded.size() - encodedSize;
        if (mbedtls_x509_crt_parse_der(&certificate, encodedStart, encodedSize) != 0
            || mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_SERVER,
                   MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)
                != 0) {
            return false;
        }
        mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &random);
        mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_NONE);
        return mbedtls_ssl_conf_own_cert(&config, &certificate, &key) == 0;
    }

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
    mbedtls_pk_context key;
    mbedtls_x509_crt certificate;
    mbedtls_ssl_config config;
};

class TlsTransport : public Transport {
public:
    TlsTransport(SocketHandle socket,
        std::shared_ptr<TlsServerIdentity> serverIdentity,
        std::optional<TransportPeerIdentity> expectedPeerIdentity)
        : _socket(socket)
        , _serverIdentity(std::move(serverIdentity))
        , _expectedPeerIdentity(expectedPeerIdentity)
    {
        mbedtls_ssl_init(&_ssl);
        mbedtls_entropy_init(&_clientEntropy);
        mbedtls_ctr_drbg_init(&_clientRandom);
        mbedtls_ssl_config_init(&_clientConfig);
    }

    ~TlsTransport() override
    {
        close();
        mbedtls_ssl_free(&_ssl);
        mbedtls_ssl_config_free(&_clientConfig);
        mbedtls_ctr_drbg_free(&_clientRandom);
        mbedtls_entropy_free(&_clientEntropy);
    }

    bool initialize()
    {
        int result = 0;
        if (_serverIdentity != nullptr) {
            result = mbedtls_ssl_setup(&_ssl, &_serverIdentity->config);
        } else {
            static constexpr unsigned char personalization[] = "fallout-ce multiplayer guest TLS";
            result = mbedtls_ctr_drbg_seed(&_clientRandom, mbedtls_entropy_func, &_clientEntropy,
                personalization, sizeof(personalization) - 1);
            if (result == 0) {
                result = mbedtls_ssl_config_defaults(&_clientConfig, MBEDTLS_SSL_IS_CLIENT,
                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
            }
            if (result == 0) {
                mbedtls_ssl_conf_min_tls_version(&_clientConfig, MBEDTLS_SSL_VERSION_TLS1_2);
                mbedtls_ssl_conf_rng(&_clientConfig, mbedtls_ctr_drbg_random, &_clientRandom);
                // The first direct-IP connection uses trust-on-first-use. A reconnect
                // pins the exact certificate digest before queued application bytes
                // are written.
                mbedtls_ssl_conf_authmode(&_clientConfig, MBEDTLS_SSL_VERIFY_NONE);
                result = mbedtls_ssl_setup(&_ssl, &_clientConfig);
            }
        }
        if (result != 0) {
            return false;
        }
        mbedtls_ssl_set_bio(&_ssl, this, sendCallback, receiveCallback, nullptr);
        return true;
    }

    TransportSendResult send(Packet packet) override
    {
        if (!_connected) {
            return TransportSendResult::Disconnected;
        }
        if (packet.size() > kMaxTransportPacketSize) {
            return TransportSendResult::PacketTooLarge;
        }
        compactOutbound();
        appendFrameSize(_outbound, packet.size());
        _outbound.insert(_outbound.end(), packet.begin(), packet.end());
        poll();
        return _connected ? TransportSendResult::Sent : TransportSendResult::Disconnected;
    }

    void poll() override
    {
        if (!_connected) {
            return;
        }
        if (!_handshakeComplete) {
            progressHandshake();
            if (!_connected || !_handshakeComplete) {
                return;
            }
        }
        flushOutbound();
        if (_connected) {
            pumpInbound();
        }
    }

    std::optional<Packet> receive() override
    {
        poll();
        if (_received.empty()) {
            return std::nullopt;
        }
        Packet packet = std::move(_received.front());
        _received.pop_front();
        return packet;
    }

    bool isConnected() const override { return _connected; }
    std::optional<TransportPeerIdentity> peerIdentity() const override { return _peerIdentity; }

    void close() override
    {
        if (_connected && _handshakeComplete) {
            mbedtls_ssl_close_notify(&_ssl);
        }
        disconnect();
        _inbound.clear();
        _outbound.clear();
        _outboundOffset = 0;
        _received.clear();
    }

private:
    static int sendCallback(void* context, const unsigned char* bytes, std::size_t size)
    {
        auto* transport = static_cast<TlsTransport*>(context);
        std::ptrdiff_t sent = sendBytes(transport->_socket, bytes, size);
        if (sent > 0) {
            return static_cast<int>(sent);
        }
        if (sent < 0 && isWouldBlock(lastSocketError())) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_NET_CONN_RESET;
    }

    static int receiveCallback(void* context, unsigned char* bytes, std::size_t size)
    {
        auto* transport = static_cast<TlsTransport*>(context);
        std::ptrdiff_t received = receiveBytes(transport->_socket, bytes, size);
        if (received > 0) {
            return static_cast<int>(received);
        }
        if (received < 0 && isWouldBlock(lastSocketError())) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return MBEDTLS_ERR_NET_CONN_RESET;
    }

    void progressHandshake()
    {
        int result = mbedtls_ssl_handshake(&_ssl);
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return;
        }
        if (result != 0) {
            disconnect();
            return;
        }
        _handshakeComplete = true;
        if (_serverIdentity != nullptr) {
            return;
        }
        const mbedtls_x509_crt* certificate = mbedtls_ssl_get_peer_cert(&_ssl);
        TransportPeerIdentity identity;
        if (certificate == nullptr || certificate->raw.p == nullptr
            || mbedtls_sha256(certificate->raw.p, certificate->raw.len, identity.data(), 0) != 0
            || (_expectedPeerIdentity.has_value() && !identitiesEqual(*_expectedPeerIdentity, identity))) {
            disconnect();
            return;
        }
        _peerIdentity = identity;
    }

    void compactOutbound()
    {
        if (_outboundOffset == 0) {
            return;
        }
        if (_outboundOffset == _outbound.size()) {
            _outbound.clear();
        } else {
            _outbound.erase(_outbound.begin(), _outbound.begin() + _outboundOffset);
        }
        _outboundOffset = 0;
    }

    void flushOutbound()
    {
        while (_connected && _outboundOffset < _outbound.size()) {
            std::size_t remaining = std::min<std::size_t>(
                _outbound.size() - _outboundOffset,
                MBEDTLS_SSL_OUT_CONTENT_LEN);
            int written = mbedtls_ssl_write(&_ssl, _outbound.data() + _outboundOffset, remaining);
            if (written > 0) {
                _outboundOffset += static_cast<std::size_t>(written);
                continue;
            }
            if (written == MBEDTLS_ERR_SSL_WANT_READ || written == MBEDTLS_ERR_SSL_WANT_WRITE) {
                return;
            }
            disconnect();
        }
        compactOutbound();
    }

    void pumpInbound()
    {
        std::array<std::uint8_t, 16384> buffer;
        for (;;) {
            int received = mbedtls_ssl_read(&_ssl, buffer.data(), buffer.size());
            if (received > 0) {
                _inbound.insert(_inbound.end(), buffer.begin(), buffer.begin() + received);
                parseFrames();
                if (!_connected) {
                    return;
                }
                continue;
            }
            if (received == MBEDTLS_ERR_SSL_WANT_READ || received == MBEDTLS_ERR_SSL_WANT_WRITE) {
                return;
            }
            disconnect();
            return;
        }
    }

    void parseFrames()
    {
        while (_inbound.size() >= sizeof(std::uint32_t)) {
            std::uint32_t frameSize = readFrameSize(_inbound);
            if (frameSize > kMaxTransportPacketSize) {
                disconnect();
                _inbound.clear();
                _received.clear();
                return;
            }
            std::size_t encodedSize = sizeof(std::uint32_t) + frameSize;
            if (_inbound.size() < encodedSize) {
                return;
            }
            _received.emplace_back(_inbound.begin() + sizeof(std::uint32_t), _inbound.begin() + encodedSize);
            _inbound.erase(_inbound.begin(), _inbound.begin() + encodedSize);
        }
    }

    void disconnect()
    {
        if (_socket != kInvalidSocket) {
            closeSocket(_socket);
            _socket = kInvalidSocket;
        }
        _connected = false;
        _outbound.clear();
        _outboundOffset = 0;
    }

    SocketHandle _socket = kInvalidSocket;
    bool _connected = true;
    bool _handshakeComplete = false;
    std::shared_ptr<TlsServerIdentity> _serverIdentity;
    std::optional<TransportPeerIdentity> _expectedPeerIdentity;
    std::optional<TransportPeerIdentity> _peerIdentity;
    mbedtls_ssl_context _ssl;
    mbedtls_entropy_context _clientEntropy;
    mbedtls_ctr_drbg_context _clientRandom;
    mbedtls_ssl_config _clientConfig;
    std::vector<std::uint8_t> _inbound;
    std::vector<std::uint8_t> _outbound;
    std::size_t _outboundOffset = 0;
    std::deque<Packet> _received;
};

std::unique_ptr<Transport> makeTlsTransport(SocketHandle socket,
    std::shared_ptr<TlsServerIdentity> serverIdentity = nullptr,
    std::optional<TransportPeerIdentity> expectedPeerIdentity = std::nullopt)
{
    setNoDelay(socket);
    if (!setNonBlocking(socket)) {
        closeSocket(socket);
        return nullptr;
    }
    auto transport = std::make_unique<TlsTransport>(socket, std::move(serverIdentity), expectedPeerIdentity);
    if (!transport->initialize()) {
        transport->close();
        return nullptr;
    }
    return transport;
}

} // namespace

struct TcpListener::Impl {
    ~Impl() { closeSocket(socket); }
    SocketHandle socket = kInvalidSocket;
    std::uint16_t port = 0;
    std::shared_ptr<TlsServerIdentity> tlsIdentity;
};

TcpListener::TcpListener(std::unique_ptr<Impl> impl)
    : _impl(std::move(impl))
{
}

TcpListener::~TcpListener() { close(); }
TcpListener::TcpListener(TcpListener&& other) noexcept
    : _impl(std::move(other._impl))
{
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept
{
    if (this != &other) {
        close();
        _impl = std::move(other._impl);
    }
    return *this;
}

std::unique_ptr<Transport> TcpListener::accept()
{
    if (!isOpen()) {
        return nullptr;
    }
    sockaddr_storage address;
    std::memset(&address, 0, sizeof(address));
#if defined(_WIN32)
    int addressSize = sizeof(address);
#else
    socklen_t addressSize = sizeof(address);
#endif
    SocketHandle socket = ::accept(_impl->socket, reinterpret_cast<sockaddr*>(&address), &addressSize);
    if (socket == kInvalidSocket) {
        return nullptr;
    }
    return makeTlsTransport(socket, _impl->tlsIdentity);
}

std::uint16_t TcpListener::port() const { return _impl != nullptr ? _impl->port : 0; }
bool TcpListener::isOpen() const { return _impl != nullptr && _impl->socket != kInvalidSocket; }

void TcpListener::close()
{
    if (_impl != nullptr && _impl->socket != kInvalidSocket) {
        closeSocket(_impl->socket);
        _impl->socket = kInvalidSocket;
    }
}

TcpListenResult listenTcp(std::uint16_t port)
{
    TcpListenResult result;
    if (!initializeSocketApi()) {
        result.error = TcpError::SocketApiUnavailable;
        return result;
    }
    auto tlsIdentity = std::make_shared<TlsServerIdentity>();
    if (!tlsIdentity->initialize()) {
        result.error = TcpError::TlsInitializationFailed;
        return result;
    }

    SocketHandle socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalidSocket) {
        result.error = TcpError::SocketCreationFailed;
        return result;
    }
    int reuseAddress = 1;
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseAddress), sizeof(reuseAddress));
    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closeSocket(socket);
        result.error = TcpError::BindFailed;
        return result;
    }
    if (::listen(socket, 2) != 0) {
        closeSocket(socket);
        result.error = TcpError::ListenFailed;
        return result;
    }

    sockaddr_in boundAddress;
    std::memset(&boundAddress, 0, sizeof(boundAddress));
#if defined(_WIN32)
    int boundAddressSize = sizeof(boundAddress);
#else
    socklen_t boundAddressSize = sizeof(boundAddress);
#endif
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&boundAddress), &boundAddressSize) != 0
        || !setNonBlocking(socket)) {
        closeSocket(socket);
        result.error = TcpError::SocketCreationFailed;
        return result;
    }
    auto impl = std::make_unique<TcpListener::Impl>();
    impl->socket = socket;
    impl->port = ntohs(boundAddress.sin_port);
    impl->tlsIdentity = std::move(tlsIdentity);
    result.listener = std::unique_ptr<TcpListener>(new TcpListener(std::move(impl)));
    return result;
}

TcpConnectResult connectTcp(const std::string& host,
    std::uint16_t port,
    std::uint32_t timeoutMilliseconds,
    std::optional<TransportPeerIdentity> expectedPeerIdentity)
{
    TcpConnectResult result;
    if (host.empty() || port == 0 || timeoutMilliseconds == 0) {
        result.error = TcpError::InvalidArgument;
        return result;
    }
    if (!initializeSocketApi()) {
        result.error = TcpError::SocketApiUnavailable;
        return result;
    }
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) != 0) {
        result.error = TcpError::AddressResolutionFailed;
        return result;
    }

    SocketHandle connectedSocket = kInvalidSocket;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        SocketHandle socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket == kInvalidSocket) {
            continue;
        }
        if (!setNonBlocking(socket)) {
            closeSocket(socket);
            continue;
        }
        int connectResult = ::connect(socket, address->ai_addr, static_cast<int>(address->ai_addrlen));
        if (connectResult == 0
            || (isConnectInProgress(lastSocketError()) && waitForConnect(socket, timeoutMilliseconds))) {
            connectedSocket = socket;
            break;
        }
        closeSocket(socket);
    }
    freeaddrinfo(addresses);

    if (connectedSocket == kInvalidSocket) {
        result.error = TcpError::ConnectFailed;
        return result;
    }
    result.transport = makeTlsTransport(connectedSocket, nullptr, expectedPeerIdentity);
    if (result.transport == nullptr) {
        result.error = TcpError::TlsInitializationFailed;
    }
    return result;
}

} // namespace multiplayer
} // namespace fallout
