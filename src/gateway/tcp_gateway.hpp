#pragma once

#include "core/ingress_event.hpp"
#include "core/order_identity.hpp"
#include "egress/client_order_event.hpp"
#include "egress/depth_snapshot.hpp"
#include "gateway/socket_poller.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace babo::gateway {

class TcpGateway {
public:
    using Submit = std::function<bool(const core::IngressEvent&)>;

    explicit TcpGateway(Submit submit, std::uint16_t port = 9000);
    ~TcpGateway();
    TcpGateway(const TcpGateway&) = delete;
    TcpGateway& operator=(const TcpGateway&) = delete;

    void pollOnce(std::chrono::milliseconds timeout);
    void route(const egress::ClientOrderEvent& event);
    void broadcastDepth(const egress::DepthSnapshot& snapshot);
    [[nodiscard]] std::size_t sessionCount() const noexcept { return sessions_.size(); }

private:
    static constexpr std::size_t kMaxSocketEvents = 128;

    struct Session {
        core::SessionId id{};
        core::ClientOrderId next_client_order_id{1};
        std::string input;
        std::string output;
        std::size_t output_offset{};
        bool watching_write{};
    };

    void acceptReady();
    void readReady(SocketHandle fd);
    void processLines(SocketHandle fd);
    void processCommand(SocketHandle fd, std::string_view line);
    void queueWrite(SocketHandle fd, std::string message);
    void flushPendingSocketWrites(SocketHandle fd);
    void closeSession(SocketHandle fd, std::string_view reason) noexcept;
    static SocketHandle createListenSocket(std::uint16_t port);

    Submit submit_;
    SocketPoller poller_;
    std::array<SocketEvent, kMaxSocketEvents> ready_events_{};
    SocketHandle listen_fd_ = kInvalidSocket;
    std::unordered_map<SocketHandle, Session> sessions_;
    std::unordered_map<core::SessionId, SocketHandle> session_fds_;
    core::SessionId next_session_id_ = 1;
    std::uint64_t next_exchange_order_sequence_ = 1;
};

} // namespace babo::gateway
