#include "gateway_connection.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace babo::client {

namespace {
#if defined(_WIN32)
std::string socketError() {
    return std::to_string(::WSAGetLastError());
}

bool interrupted() {
    return ::WSAGetLastError() == WSAEINTR;
}

void closeSocket(SOCKET fd) noexcept {
    if (fd != INVALID_SOCKET) ::closesocket(fd);
}
#else
std::string socketError() {
    return std::strerror(errno);
}

bool interrupted() {
    return errno == EINTR;
}

void closeSocket(int fd) noexcept {
    if (fd >= 0) ::close(fd);
}
#endif
} // namespace

GatewayConnection::GatewayConnection(std::string_view host,
                                     std::uint16_t port) {
#if defined(_WIN32)
    WSADATA data{};
    const int startup = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (startup != 0) {
        throw std::system_error(startup, std::system_category(), "WSAStartup");
    }
    wsa_started_ = true;
#endif

    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (
#if defined(_WIN32)
        fd_ == INVALID_SOCKET
#else
        fd_ < 0
#endif
    ) {
#if defined(_WIN32)
        if (wsa_started_) ::WSACleanup();
        wsa_started_ = false;
#endif
        throw std::runtime_error("socket: " + socketError());
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    const std::string hostString(host);
    if (::inet_pton(AF_INET, hostString.c_str(), &address.sin_addr) != 1) {
        closeSocket(fd_);
#if defined(_WIN32)
        fd_ = INVALID_SOCKET;
        if (wsa_started_) ::WSACleanup();
        wsa_started_ = false;
#else
        fd_ = -1;
#endif
        throw std::runtime_error("invalid IPv4 Gateway address");
    }
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) <
        0) {
        const std::string reason = socketError();
        closeSocket(fd_);
#if defined(_WIN32)
        fd_ = INVALID_SOCKET;
        if (wsa_started_) ::WSACleanup();
        wsa_started_ = false;
#else
        fd_ = -1;
#endif
        throw std::runtime_error("connect to 127.0.0.1:" +
                                 std::to_string(port) + ": " + reason);
    }
}

GatewayConnection::~GatewayConnection() {
    closeSocket(fd_);
#if defined(_WIN32)
    if (wsa_started_) ::WSACleanup();
#endif
}

void GatewayConnection::run(std::stop_token stopToken,
                            const LineHandler& onLine,
                            const DisconnectHandler& onDisconnect) {
    std::string pending;
    pending.reserve(16 * 1024);
    char buffer[4096];

    while (!stopToken.stop_requested()) {
        const auto received = ::recv(fd_, buffer, static_cast<int>(sizeof(buffer)), 0);
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
        if (interrupted()) continue;
        if (!stopToken.stop_requested()) onDisconnect(socketError());
        return;
    }
}

void GatewayConnection::stop() noexcept {
    if (
#if defined(_WIN32)
        fd_ != INVALID_SOCKET
#else
        fd_ >= 0
#endif
    ) {
#if defined(_WIN32)
        ::shutdown(fd_, SD_BOTH);
#else
        ::shutdown(fd_, SHUT_RDWR);
#endif
    }
}

} // namespace babo::client
