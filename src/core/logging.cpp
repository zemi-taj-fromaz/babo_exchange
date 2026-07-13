#include "core/logging.hpp"

#include <spdlog/sinks/basic_file_sink.h>

#include <atomic>

namespace babo::log {

namespace {
std::atomic<bool> g_initialized{false};
} // namespace

void init(spdlog::level::level_enum level) {
    bool expected = false;
    if (!g_initialized.compare_exchange_strong(expected, true)) {
        return; // already initialized
    }

    // File sink keeps the terminal clean. spdlog creates the logs/ directory if
    // it doesn't exist. `true` = truncate on each run (drop for append).
    auto logger = spdlog::basic_logger_mt("babo", "logs/babo.log", true);

    // [time] [level] [thread] message  — thread id matters here because the
    // whole design is multi-threaded (feed / matching / egress). No color codes
    // in a file sink.
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [t:%t] %v");
    logger->set_level(level);
    logger->flush_on(spdlog::level::warn);

    spdlog::set_default_logger(logger);

    spdlog::info("logging initialized (level={})",
                 spdlog::level::to_string_view(level));
}

} // namespace babo::log
