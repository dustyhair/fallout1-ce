#include "multiplayer/tcp_transport.h"

#include <utility>

namespace fallout {
namespace multiplayer {

struct TcpListener::Impl {
};

TcpListener::TcpListener(std::unique_ptr<Impl> impl)
    : _impl(std::move(impl))
{
}

TcpListener::~TcpListener() = default;
TcpListener::TcpListener(TcpListener&& other) noexcept = default;
TcpListener& TcpListener::operator=(TcpListener&& other) noexcept = default;

std::unique_ptr<Transport> TcpListener::accept() { return nullptr; }
std::uint16_t TcpListener::port() const { return 0; }
std::optional<TransportPeerIdentity> TcpListener::identity() const { return std::nullopt; }
bool TcpListener::isOpen() const { return false; }
void TcpListener::close() { _impl.reset(); }

TcpListenResult listenTcp(std::uint16_t)
{
    return { TcpError::SocketApiUnavailable, nullptr };
}

TcpConnectResult connectTcp(const std::string& host,
    std::uint16_t port,
    std::uint32_t timeoutMilliseconds,
    std::optional<TransportPeerIdentity>)
{
    if (host.empty() || port == 0 || timeoutMilliseconds == 0) {
        return { TcpError::InvalidArgument, nullptr };
    }
    return { TcpError::SocketApiUnavailable, nullptr };
}

} // namespace multiplayer
} // namespace fallout
