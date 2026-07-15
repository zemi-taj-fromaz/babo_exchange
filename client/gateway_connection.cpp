#include "gateway_connection.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace babo::client {

GatewayConnection::GatewayConnection(std::string_view host,
                                     std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    const std::string hostString(host);
    if (::inet_pton(AF_INET, hostString.c_str(), &address.sin_addr) != 1) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("invalid IPv4 Gateway address");
    }
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) <
        0) {
        const std::string reason = std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("connect to 127.0.0.1:" +
                                 std::to_string(port) + ": " + reason);
    }
}

GatewayConnection::~GatewayConnection() {
    if (fd_ >= 0) ::close(fd_);
}

void GatewayConnection::run(std::stop_token stopToken,
                            const LineHandler& onLine,
                            const DisconnectHandler& onDisconnect) {
    std::string pending;
    pending.reserve(16 * 1024);
    char buffer[4096];

    while (!stopToken.stop_requested()) {
        const auto received = ::recv(fd_, buffer, sizeof(buffer), 0);
        if (received > 0) {
            pending.append(buffer, static_cast<std::size_t>(received));
            for (;;) {
                const auto newline = pending.find('\n');
                if (newline == std::string::npos) break;
                std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                onLine(line);
            }
            if (pending.size() > 1024 * 1024) {
                onDisconnect("Gateway input buffer limit exceeded");
                return;
            }
            continue;
        }
        if (received == 0) {
            if (!stopToken.stop_requested()) onDisconnect("Gateway disconnected");
            return;
        }
        if (errno == EINTR) continue;
        if (!stopToken.stop_requested()) onDisconnect(std::strerror(errno));
        return;
    }
}

void GatewayConnection::stop() noexcept {
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
}

} // namespace babo::client
