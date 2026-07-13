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

# ---- mbedTLS + IXWebSocket (Coinbase live feed) ----------------------------
# mbedTLS from the official RELEASE TARBALL: self-contained (no git submodules,
# no Python codegen). Built with our own toolchain, so the ABI always matches
# (we have mixed Clang / llvm-mingw toolchains) and it's identical on all of
# Win/macOS/Linux. TLS backend for the WebSocket, so no system OpenSSL needed.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)  # mbedtls: skip its sample apps
set(ENABLE_TESTING  OFF CACHE BOOL "" FORCE)  # mbedtls: skip its test suite
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(mbedtls)

# IXWebSocket's bundled FindMbedTLS.cmake does find_library(mbedtls ...), which
# can't see our FetchContent targets. find_*() skips a cache var that's already
# set, so pre-seed the exact vars it checks and point them at the built targets.
set(MBEDTLS_INCLUDE_DIRS "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(MBEDTLS_VERSION_GREATER_THAN_3 "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(MBEDTLS_LIBRARY    mbedtls    CACHE STRING "" FORCE)
set(MBEDX509_LIBRARY   mbedx509   CACHE STRING "" FORCE)
set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)

set(USE_TLS           ON  CACHE BOOL "" FORCE)
set(USE_MBED_TLS      ON  CACHE BOOL "" FORCE)
set(USE_ZLIB          OFF CACHE BOOL "" FORCE)  # no permessage-deflate; skip zlib
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE) # in-tree link; no install/export
FetchContent_Declare(ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG        v11.4.5
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(ixwebsocket)

# ---- babobook (the matching core) ------------------------------------------
# Vendored copy of the matching-engine library. Multiplatform, network-agnostic,
# header-only + one .cpp; exposes babo::book::matching_book. This is the core we
# wrap; the exchange is the system around it.
add_subdirectory(libs/babobook)
add_subdirectory(third_party/rigtorp/SPSCQueue)

add_subdirectory(src)
