#include "core/logging.hpp"
#include "core/main_process.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

namespace {

// Set by the signal handler on Ctrl+C / SIGTERM. The run loop polls it and
// shuts down cleanly instead of being hard-killed mid-work. Must be lock-free
// and async-signal-safe — std::atomic<bool> qualifies; almost nothing else does
// (no logging, no allocation, no mutexes inside a signal handler).
std::atomic<bool> g_running{true};

extern "C" void on_signal(int /*sig*/) {
    g_running.store(false, std::memory_order_relaxed);
}

} // namespace

int main() {
    babo::log::init();
    spdlog::info("babo_exchange up");

    std::signal(SIGINT, on_signal);   // Ctrl+C
    std::signal(SIGTERM, on_signal);  // kill / service stop

    // Construct the process: this immediately spawns networkThread. When
    // `process` goes out of scope at the end of main, its jthread member is
    // destroyed, which auto-requests stop + joins the thread. No manual stop
    // token handling needed here.
    babo::MainProcess process;

    spdlog::info("running — press Ctrl+C to stop");

    // Perpetual run loop: fetch -> process -> emit -> repeat. Right now it just
    // ticks on a timer; the real work (feed source -> ingress ring -> matching
    // -> egress) slots in here later.
    std::uint64_t tick = 0;
    while (g_running.load(std::memory_order_relaxed)) {
        //spdlog::info("tick {}", tick++);

        // Placeholder for real work / pacing. Replace with the feed drain once
        // the ingress ring exists.
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    spdlog::info("babo_exchange shutting down cleanly");
    return 0;
}
