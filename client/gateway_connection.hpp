#pragma once

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string_view>

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
    int fd_ = -1;
};

} // namespace babo::client
