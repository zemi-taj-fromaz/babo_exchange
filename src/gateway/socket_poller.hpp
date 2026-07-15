#pragma once

#include <chrono>
#include <span>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <unordered_set>
#endif

namespace babo::gateway {

#if defined(_WIN32)
using SocketHandle = SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

struct SocketEvent {
    SocketHandle fd = kInvalidSocket;
    bool readable = false;
    bool writable = false;
    bool error = false;
};

// Small readiness-poller seam: kqueue on macOS/BSD, epoll on Linux, select on
// Windows. The Windows path is good enough for the local gateway/client slice.
class SocketPoller {
public:
    SocketPoller();
    ~SocketPoller();
    SocketPoller(const SocketPoller&) = delete;
    SocketPoller& operator=(const SocketPoller&) = delete;

    void add(SocketHandle fd);
    void setWritable(SocketHandle fd, bool enabled);
    void remove(SocketHandle fd) noexcept;
    int wait(std::span<SocketEvent> events, std::chrono::milliseconds timeout);

private:
#if defined(_WIN32)
    std::unordered_set<SocketHandle> read_fds_;
    std::unordered_set<SocketHandle> write_fds_;
    bool wsa_started_ = false;
#else
    int fd_ = -1;
#endif
};

} // namespace babo::gateway
