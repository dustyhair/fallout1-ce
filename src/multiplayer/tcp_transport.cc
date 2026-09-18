#include "multiplayer/tcp_transport.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstring>
#include <deque>
#include <utility>
#include <vector>

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

class TcpTransport : public Transport {
public:
    explicit TcpTransport(SocketHandle socket)
        : _socket(socket)
    {
    }

    ~TcpTransport() override
    {
        close();
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
        flushOutbound();
        return _connected ? TransportSendResult::Sent : TransportSendResult::Disconnected;
    }

    void poll() override
    {
        if (_connected) {
            flushOutbound();
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

    bool isConnected() const override
    {
        return _connected;
    }

    void close() override
    {
        disconnect();
        _inbound.clear();
        _outbound.clear();
        _outboundOffset = 0;
        _received.clear();
    }

private:
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
            std::ptrdiff_t sent = sendBytes(_socket, _outbound.data() + _outboundOffset, _outbound.size() - _outboundOffset);
            if (sent > 0) {
                _outboundOffset += static_cast<std::size_t>(sent);
                continue;
            }
            if (sent < 0 && isWouldBlock(lastSocketError())) {
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
            std::ptrdiff_t received = receiveBytes(_socket, buffer.data(), buffer.size());
            if (received > 0) {
                _inbound.insert(_inbound.end(), buffer.begin(), buffer.begin() + received);
                parseFrames();
                if (!_connected) {
                    return;
                }
                continue;
            }
            if (received < 0 && isWouldBlock(lastSocketError())) {
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
    std::vector<std::uint8_t> _inbound;
    std::vector<std::uint8_t> _outbound;
    std::size_t _outboundOffset = 0;
    std::deque<Packet> _received;
};

std::unique_ptr<Transport> makeTcpTransport(SocketHandle socket)
{
    setNoDelay(socket);
    if (!setNonBlocking(socket)) {
        closeSocket(socket);
        return nullptr;
    }
    return std::make_unique<TcpTransport>(socket);
}

} // namespace

struct TcpListener::Impl {
    ~Impl()
    {
        closeSocket(socket);
    }

    SocketHandle socket = kInvalidSocket;
    std::uint16_t port = 0;
};

TcpListener::TcpListener(std::unique_ptr<Impl> impl)
    : _impl(std::move(impl))
{
}

TcpListener::~TcpListener()
{
    close();
}

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
    return makeTcpTransport(socket);
}

std::uint16_t TcpListener::port() const
{
    return _impl != nullptr ? _impl->port : 0;
}

bool TcpListener::isOpen() const
{
    return _impl != nullptr && _impl->socket != kInvalidSocket;
}

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

    if (::listen(socket, 1) != 0) {
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
    result.listener = std::unique_ptr<TcpListener>(new TcpListener(std::move(impl)));
    return result;
}

TcpConnectResult connectTcp(const std::string& host, std::uint16_t port, std::uint32_t timeoutMilliseconds)
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

    result.transport = makeTcpTransport(connectedSocket);
    if (result.transport == nullptr) {
        result.error = TcpError::SocketCreationFailed;
    }
    return result;
}

} // namespace multiplayer
} // namespace fallout
