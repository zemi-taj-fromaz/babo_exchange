#pragma once

#include <chrono>
#include <span>

namespace babo::gateway {

struct SocketEvent {
    int fd = -1;
    bool readable = false;
    bool writable = false;
    bool error = false;
};

// Small readiness-poller seam: kqueue on macOS/BSD, epoll on Linux.
class SocketPoller {
public:
    SocketPoller();
    ~SocketPoller();
    SocketPoller(const SocketPoller&) = delete;
    SocketPoller& operator=(const SocketPoller&) = delete;

    void add(int fd);
    void setWritable(int fd, bool enabled);
    void remove(int fd) noexcept;
    int wait(std::span<SocketEvent> events, std::chrono::milliseconds timeout);

private:
    int fd_ = -1;
};

} // namespace babo::gateway
