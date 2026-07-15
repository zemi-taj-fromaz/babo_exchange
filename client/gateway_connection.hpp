#pragma once

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace babo::client {

// Read-only TCP connection used by the first babo_client slice. It owns one
// connection to the exchange Gateway and emits complete newline-framed records.
class GatewayConnection {
public:
    using LineHandler = std::function<void(std::string_view)>;
    using DisconnectHandler = std::function<void(std::string_view)>;

    GatewayConnection(std::string_view host, std::uint16_t port);
    ~GatewayConnection();
    GatewayConnection(const GatewayConnection&) = delete;
    GatewayConnection& operator=(const GatewayConnection&) = delete;

    void run(std::stop_token stopToken, const LineHandler& onLine,
             const DisconnectHandler& onDisconnect);
    void stop() noexcept;

private:
#if defined(_WIN32)
    SOCKET fd_ = INVALID_SOCKET;
    bool wsa_started_ = false;
#else
    int fd_ = -1;
#endif
};

} // namespace babo::client
