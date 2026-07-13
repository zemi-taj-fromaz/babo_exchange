#pragma once

#include <spdlog/spdlog.h>

namespace babo::log {

// Initialize the global logging subsystem. Call once, early in main(), before
// any other thread is spawned. Safe to call more than once (idempotent).
//
// This sets up the default logger used by the plain spdlog::info/warn/error
// free functions throughout the codebase. It does NOT install any logging on
// the hot path — matching-thread diagnostics, if ever needed, must go through
// a separate lock-free/async sink so they never block the single writer.
void init(spdlog::level::level_enum level = spdlog::level::info);

} // namespace babo::log
