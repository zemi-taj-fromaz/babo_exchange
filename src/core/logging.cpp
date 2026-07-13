#include "core/logging.hpp"

#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

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

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    auto console_logger = std::make_shared<spdlog::logger>("babo", console_sink);
    console_logger->set_level(level);
    console_logger->flush_on(spdlog::level::warn);
    spdlog::register_logger(console_logger);
    spdlog::set_default_logger(console_logger);

    auto file_sink =
        std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/babo.log", true);
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [t:%t] %v");

    auto file_logger = std::make_shared<spdlog::logger>("babo_sync", file_sink);
    file_logger->set_level(spdlog::level::info);
    file_logger->flush_on(spdlog::level::info);
    spdlog::register_logger(file_logger);

    spdlog::info("logging initialized (level={})",
                 spdlog::level::to_string_view(level));
}

} // namespace babo::log
