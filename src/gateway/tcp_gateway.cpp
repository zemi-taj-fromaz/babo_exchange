#include "gateway/tcp_gateway.hpp"

#include "feed/fixed_point.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <climits>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <basetsd.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace babo::gateway {

namespace {
constexpr std::size_t kMaxInputBytes = 16 * 1024;
constexpr std::size_t kMaxOutputBytes = 1024 * 1024;
constexpr std::size_t kMaxCommandsPerRead = 64;

#if defined(_WIN32)
std::string socketError() {
    return std::to_string(::WSAGetLastError());
}

bool wouldBlock() {
    const int error = ::WSAGetLastError();
    return error == WSAEWOULDBLOCK;
}

bool interrupted() {
    return ::WSAGetLastError() == WSAEINTR;
}

void closeSocket(SocketHandle fd) noexcept {
    if (fd != kInvalidSocket) ::closesocket(fd);
}

void setNonBlocking(SocketHandle fd) {
    u_long enabled = 1;
    if (::ioctlsocket(fd, FIONBIO, &enabled) != 0) {
        throw std::runtime_error("ioctlsocket: " + socketError());
    }
}
#else
std::string socketError() {
    return std::strerror(errno);
}

bool wouldBlock() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

bool interrupted() {
    return errno == EINTR;
}

void closeSocket(SocketHandle fd) noexcept {
    if (fd != kInvalidSocket) ::close(fd);
}

void setNonBlocking(SocketHandle fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error(std::string("fcntl: ") + std::strerror(errno));
    }
}
#endif

std::array<std::string_view, 4> split(std::string_view line, std::size_t& count) {
    std::array<std::string_view, 4> parts{};
    count = 0;
    while (!line.empty() && count < parts.size()) {
        const auto start = line.find_first_not_of(" \t\r");
        if (start == std::string_view::npos) break;
        line.remove_prefix(start);
        const auto end = line.find_first_of(" \t\r");
        parts[count++] = line.substr(0, end);
        if (end == std::string_view::npos) break;
        line.remove_prefix(end);
    }
    return parts;
}

bool parseUint64(std::string_view text, std::uint64_t& value) {
    const auto* first = text.data();
    const auto* last = first + text.size();
    const auto [end, error] = std::from_chars(first, last, value);
    return error == std::errc{} && end == last;
}

const char* eventName(egress::ClientOrderEventType type) {
    using T = egress::ClientOrderEventType;
    switch (type) {
    case T::Accepted: return "ACCEPTED";
    case T::Rejected: return "REJECTED";
    case T::Cancelled: return "CANCELLED";
    case T::CancelRejected: return "CANCEL_REJECTED";
    case T::Replaced: return "REPLACED";
    case T::ReplaceRejected: return "REPLACE_REJECTED";
    case T::Fill: return "FILL";
    }
    return "UNKNOWN";
}
} // namespace

SocketHandle TcpGateway::createListenSocket(std::uint16_t port) {
    const SocketHandle fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) throw std::runtime_error("socket failed");
    try {
        const int enabled = 1;
#if defined(_WIN32)
        const auto* enabledPtr = reinterpret_cast<const char*>(&enabled);
#else
        const auto* enabledPtr = &enabled;
#endif
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, enabledPtr,
                         sizeof(enabled)) < 0) {
            throw std::runtime_error("setsockopt failed");
        }
        setNonBlocking(fd);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            throw std::runtime_error("bind: " + socketError());
        }
        if (::listen(fd, 128) < 0) throw std::runtime_error("listen failed");
        return fd;
    } catch (...) {
        closeSocket(fd);
        throw;
    }
}

TcpGateway::TcpGateway(Submit submit, std::uint16_t port)
    : submit_(std::move(submit)), listen_fd_(createListenSocket(port)) {
    poller_.add(listen_fd_);
    spdlog::info("gateway: listening on 127.0.0.1:{}", port);
}

TcpGateway::~TcpGateway() {
    for (const auto& [fd, session] : sessions_) closeSocket(fd);
    if (listen_fd_ != kInvalidSocket) closeSocket(listen_fd_);
}

void TcpGateway::pollOnce(std::chrono::milliseconds timeout) {
    std::array<SocketEvent, 128> events{};
    const int count = poller_.wait(events, timeout);
    for (int i = 0; i < count; ++i) {
        const auto event = events[i];
        if (event.fd == listen_fd_) {
            if (event.readable) acceptReady();
            continue;
        }
        if (!sessions_.contains(event.fd)) continue;
        if (event.readable) readReady(event.fd);
        if (!sessions_.contains(event.fd)) continue;
        if (event.writable) flushPendingSocketWrites(event.fd);
        if (event.error && sessions_.contains(event.fd)) {
            closeSession(event.fd, "socket closed");
        }
    }
}

void TcpGateway::acceptReady() {
    for (;;) {
        const SocketHandle fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd == kInvalidSocket) {
            if (wouldBlock()) return;
            if (interrupted()) continue;
            throw std::runtime_error("accept: " + socketError());
        }
        try {
            setNonBlocking(fd);
#if defined(__APPLE__)
            const int enabled = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
            const auto sessionId = next_session_id_++;
            sessions_.emplace(fd, Session{sessionId});
            session_fds_.emplace(sessionId, fd);
            poller_.add(fd);
            queueWrite(fd, "SESSION " + std::to_string(sessionId) +
                               "\nCOMMANDS buy <qty> <price> | sell <qty> <price> | cancel <order_id>\n");
            spdlog::info("gateway: session {} connected (fd={})", sessionId,
                         static_cast<std::uint64_t>(fd));
        } catch (...) {
            closeSocket(fd);
            throw;
        }
    }
}

void TcpGateway::readReady(SocketHandle fd) {
    auto it = sessions_.find(fd);
    if (it == sessions_.end()) return;
    char buffer[4096];
    for (;;) {
        const auto received = ::recv(fd, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received > 0) {
            it->second.input.append(buffer, static_cast<std::size_t>(received));
            if (it->second.input.size() > kMaxInputBytes) {
                closeSession(fd, "input buffer limit");
                return;
            }
            continue;
        }
        if (received == 0) {
            closeSession(fd, "peer disconnected");
            return;
        }
        if (interrupted()) continue;
        if (wouldBlock()) break;
        closeSession(fd, socketError());
        return;
    }
    processLines(fd);
}

void TcpGateway::processLines(SocketHandle fd) {
    std::size_t processed = 0;
    while (processed < kMaxCommandsPerRead) {
        auto it = sessions_.find(fd);
        if (it == sessions_.end()) return;
        const auto newline = it->second.input.find('\n');
        if (newline == std::string::npos) return;
        std::string line = it->second.input.substr(0, newline);
        it->second.input.erase(0, newline + 1);
        processCommand(fd, line);
        ++processed;
    }
}

void TcpGateway::processCommand(SocketHandle fd, std::string_view line) {
    auto it = sessions_.find(fd);
    if (it == sessions_.end()) return;
    std::size_t count = 0;
    const auto parts = split(line, count);
    if (count == 3 && (parts[0] == "buy" || parts[0] == "sell")) {
        try {
            const auto qty = feed::parseQtyLots(parts[1]);
            const auto price = feed::parsePriceTicks(parts[2]);
            if (qty == 0 || price == 0) throw std::invalid_argument("zero");
            auto& session = it->second;
            core::IngressEvent event;
            event.source = core::IngressSource::Client;
            event.type = core::IngressEventType::New;
            event.order_id =
                core::makeClientOrderId(next_exchange_order_sequence_++);
            event.session_id = session.id;
            event.client_order_id = session.next_client_order_id++;
            event.price_ticks = price;
            event.qty_lots = qty;
            event.side = parts[0] == "buy" ? 'B' : 'S';
            if (!submit_(event)) queueWrite(fd, "ERROR ingress_busy\n");
        } catch (const std::exception&) {
            queueWrite(fd, "ERROR usage: buy|sell <qty> <price>\n");
        }
        return;
    }
    if (count == 2 && parts[0] == "cancel") {
        std::uint64_t orderId = 0;
        if (!parseUint64(parts[1], orderId)) {
            queueWrite(fd, "ERROR usage: cancel <order_id>\n");
            return;
        }
        core::IngressEvent event;
        event.source = core::IngressSource::Client;
        event.type = core::IngressEventType::Cancel;
        event.order_id = orderId;
        event.session_id = it->second.id;
        if (!submit_(event)) queueWrite(fd, "ERROR ingress_busy\n");
        return;
    }
    queueWrite(fd, "ERROR unknown_command\n");
}

void TcpGateway::route(const egress::ClientOrderEvent& event) {
    const auto target = session_fds_.find(event.target_session_id);
    if (target == session_fds_.end()) return;
    std::ostringstream line;
    line << eventName(event.type) << " seq=" << event.event_sequence
         << " client_order_id=" << event.client_order_id
         << " order_id=" << event.exchange_order_id
         << " price_ticks=" << event.price_ticks << " qty_lots=" << event.qty_lots
         << " remaining_qty_lots=" << event.remaining_qty_lots
         << " role=" << static_cast<int>(event.fill_role)
         << " reason=" << static_cast<int>(event.reject_reason) << '\n';
    queueWrite(target->second, line.str());
}

void TcpGateway::broadcastDepth(const egress::DepthSnapshot& snapshot) {
    std::ostringstream line;
    line << "DEPTH seq=" << snapshot.sequence;
    for (const auto& level : snapshot.bids) {
        line << " B=" << level.price_ticks << ',' << level.qty_lots << ','
             << level.order_count;
    }
    for (const auto& level : snapshot.asks) {
        line << " A=" << level.price_ticks << ',' << level.qty_lots << ','
             << level.order_count;
    }
    line << '\n';
    const auto message = line.str();
    std::vector<SocketHandle> fds;
    fds.reserve(sessions_.size());
    for (const auto& [fd, session] : sessions_) fds.push_back(fd);
    for (const auto fd : fds) queueWrite(fd, message);
}

void TcpGateway::queueWrite(SocketHandle fd, std::string message) {
    auto it = sessions_.find(fd);
    if (it == sessions_.end()) return;
    auto& session = it->second;
    if (session.output.size() - session.output_offset + message.size() >
        kMaxOutputBytes) {
        closeSession(fd, "slow consumer");
        return;
    }
    if (session.output_offset != 0) {
        session.output.erase(0, session.output_offset);
        session.output_offset = 0;
    }
    session.output += message;
    flushPendingSocketWrites(fd);
}

void TcpGateway::flushPendingSocketWrites(SocketHandle fd) {
    auto it = sessions_.find(fd);
    if (it == sessions_.end()) return;
    auto& session = it->second;
    while (session.output_offset < session.output.size()) {
        const char* data = session.output.data() + session.output_offset;
        const auto remaining = session.output.size() - session.output_offset;
        const auto sent =
            ::send(fd, data, static_cast<int>(std::min<std::size_t>(
                                 remaining, static_cast<std::size_t>(INT_MAX))),
                   0);
        if (sent > 0) {
            session.output_offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && interrupted()) continue;
        if (sent < 0 && wouldBlock()) {
            if (!session.watching_write) {
                poller_.setWritable(fd, true);
                session.watching_write = true;
            }
            return;
        }
        closeSession(fd, sent == 0 ? "write closed" : socketError());
        return;
    }
    session.output.clear();
    session.output_offset = 0;
    if (session.watching_write) {
        poller_.setWritable(fd, false);
        session.watching_write = false;
    }
}

void TcpGateway::closeSession(SocketHandle fd, std::string_view reason) noexcept {
    const auto it = sessions_.find(fd);
    if (it == sessions_.end()) return;
    spdlog::info("gateway: session {} disconnected ({})", it->second.id, reason);
    session_fds_.erase(it->second.id);
    poller_.remove(fd);
    closeSocket(fd);
    sessions_.erase(it);
}

} // namespace babo::gateway
