cmake_minimum_required(VERSION 3.20)

project(babo_exchange
    VERSION 0.1.0
    DESCRIPTION "Low-latency matching engine + simulated exchange"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)

# ---- spdlog ----------------------------------------------------------------
# Header-only would work, but the compiled lib keeps hot-path translation units
# from recompiling fmt every time. Static so there is no runtime dependency.
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.3
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(spdlog)

# ---- nlohmann/json ---------------------------------------------------------
# Header-only JSON, used to parse the Coinbase REST L3 snapshot (bootstrap, not
# hot path — ergonomics over raw parse speed here).
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# ---- babobook (the matching core) ------------------------------------------
# Vendored copy of the matching-engine library. Multiplatform, network-agnostic,
# header-only + one .cpp; exposes babo::book::matching_book. This is the core we
# wrap; the exchange is the system around it.
add_subdirectory(libs/babobook)

add_subdirectory(src)
