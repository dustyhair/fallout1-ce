#ifndef FALLOUT_MULTIPLAYER_TCP_TRANSPORT_H_
#define FALLOUT_MULTIPLAYER_TCP_TRANSPORT_H_

#include <cstdint>
#include <memory>
#include <string>

#include "multiplayer/transport.h"

namespace fallout {
namespace multiplayer {

enum class TcpError {
    None,
    InvalidArgument,
    SocketApiUnavailable,
    AddressResolutionFailed,
    SocketCreationFailed,
    BindFailed,
    ListenFailed,
    ConnectFailed,
};

struct TcpListenResult;

class TcpListener {
public:
    ~TcpListener();

    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    std::unique_ptr<Transport> accept();
    std::uint16_t port() const;
    bool isOpen() const;
    void close();

private:
    struct Impl;

    explicit TcpListener(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> _impl;

    friend struct TcpListenResult;
    friend TcpListenResult listenTcp(std::uint16_t port);
};

struct TcpListenResult {
    TcpError error = TcpError::None;
    std::unique_ptr<TcpListener> listener;

    explicit operator bool() const
    {
        return error == TcpError::None && listener != nullptr;
    }
};

struct TcpConnectResult {
    TcpError error = TcpError::None;
    std::unique_ptr<Transport> transport;

    explicit operator bool() const
    {
        return error == TcpError::None && transport != nullptr;
    }
};

TcpListenResult listenTcp(std::uint16_t port);
TcpConnectResult connectTcp(const std::string& host, std::uint16_t port, std::uint32_t timeoutMilliseconds = 5000);

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_TCP_TRANSPORT_H_ */
