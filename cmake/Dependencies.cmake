include_guard(GLOBAL)

include(FetchContent)

function(babo_fetch_exchange_dependencies)
    # spdlog: use the compiled static library to avoid recompiling fmt in each
    # exchange translation unit and to keep deployment self-contained.
    set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        v1.15.3
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(spdlog)

    # Header-only JSON parsing for external snapshots and WebSocket payloads.
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(nlohmann_json)

    # Build a pinned mbedTLS release for IXWebSocket so every platform uses the
    # same TLS implementation without requiring a system OpenSSL installation.
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(mbedtls
        URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2)
    FetchContent_MakeAvailable(mbedtls)

    # IXWebSocket's finder searches for installed library files and cannot see
    # FetchContent targets directly. Pre-seed its variables with our targets.
    set(MBEDTLS_INCLUDE_DIRS "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
    set(MBEDTLS_VERSION_GREATER_THAN_3 "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
    set(MBEDTLS_LIBRARY mbedtls CACHE STRING "" FORCE)
    set(MBEDX509_LIBRARY mbedx509 CACHE STRING "" FORCE)
    set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)

    set(USE_TLS ON CACHE BOOL "" FORCE)
    set(USE_MBED_TLS ON CACHE BOOL "" FORCE)
    set(USE_ZLIB OFF CACHE BOOL "" FORCE)
    set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(ixwebsocket
        GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
        GIT_TAG        v11.4.5
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(ixwebsocket)
endfunction()

function(babo_fetch_client_dependencies)
    set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(ftxui
        GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
        GIT_TAG        v6.1.9
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(ftxui)
endfunction()
