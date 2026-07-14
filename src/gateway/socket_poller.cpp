#include "gateway/socket_poller.hpp"

#include <algorithm>
#include <cerrno>
#include <system_error>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#include <unistd.h>
#elif defined(__linux__)
#include <sys/epoll.h>
#include <unistd.h>
#else
#error "SocketPoller currently supports kqueue and epoll platforms"
#endif

namespace babo::gateway {

namespace {
[[noreturn]] void fail(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}
} // namespace

SocketPoller::SocketPoller() {
#if defined(__APPLE__) || defined(__FreeBSD__)
    fd_ = ::kqueue();
#else
    fd_ = ::epoll_create1(EPOLL_CLOEXEC);
#endif
    if (fd_ < 0) fail("create socket poller");
}

SocketPoller::~SocketPoller() {
    if (fd_ >= 0) ::close(fd_);
}

void SocketPoller::add(int fd) {
#if defined(__APPLE__) || defined(__FreeBSD__)
    struct kevent change;
    EV_SET(&change, fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    if (::kevent(fd_, &change, 1, nullptr, 0, nullptr) < 0) fail("kevent add");
#else
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP;
    event.data.fd = fd;
    if (::epoll_ctl(fd_, EPOLL_CTL_ADD, fd, &event) < 0) fail("epoll add");
#endif
}

void SocketPoller::setWritable(int fd, bool enabled) {
#if defined(__APPLE__) || defined(__FreeBSD__)
    struct kevent change;
    EV_SET(&change, fd, EVFILT_WRITE, enabled ? EV_ADD : EV_DELETE, 0, 0,
           nullptr);
    if (::kevent(fd_, &change, 1, nullptr, 0, nullptr) < 0 &&
        !(errno == ENOENT && !enabled)) {
        fail("kevent writable");
    }
#else
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP | (enabled ? EPOLLOUT : 0);
    event.data.fd = fd;
    if (::epoll_ctl(fd_, EPOLL_CTL_MOD, fd, &event) < 0) fail("epoll modify");
#endif
}

void SocketPoller::remove(int fd) noexcept {
#if defined(__APPLE__) || defined(__FreeBSD__)
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    ::kevent(fd_, changes, 2, nullptr, 0, nullptr);
#else
    ::epoll_ctl(fd_, EPOLL_CTL_DEL, fd, nullptr);
#endif
}

int SocketPoller::wait(std::span<SocketEvent> events,
                       std::chrono::milliseconds timeout) {
#if defined(__APPLE__) || defined(__FreeBSD__)
    struct timespec ts{timeout.count() / 1000,
                       (timeout.count() % 1000) * 1'000'000};
    struct kevent ready[128];
    const int capacity = static_cast<int>(std::min<std::size_t>(events.size(), 128));
    const int count = ::kevent(fd_, nullptr, 0, ready, capacity, &ts);
    if (count < 0) {
        if (errno == EINTR) return 0;
        fail("kevent wait");
    }
    for (int i = 0; i < count; ++i) {
        events[i] = SocketEvent{static_cast<int>(ready[i].ident),
                                ready[i].filter == EVFILT_READ,
                                ready[i].filter == EVFILT_WRITE,
                                (ready[i].flags & (EV_ERROR | EV_EOF)) != 0};
    }
    return count;
#else
    epoll_event ready[128];
    const int capacity = static_cast<int>(std::min<std::size_t>(events.size(), 128));
    const int count = ::epoll_wait(fd_, ready, capacity,
                                   static_cast<int>(timeout.count()));
    if (count < 0) {
        if (errno == EINTR) return 0;
        fail("epoll wait");
    }
    for (int i = 0; i < count; ++i) {
        const auto flags = ready[i].events;
        events[i] = SocketEvent{ready[i].data.fd, (flags & EPOLLIN) != 0,
                                (flags & EPOLLOUT) != 0,
                                (flags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0};
    }
    return count;
#endif
}

} // namespace babo::gateway
