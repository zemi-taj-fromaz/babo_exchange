cmake_minimum_required(VERSION 3.20)

project(babo_exchange
    VERSION 0.1.0
    DESCRIPTION "Custom low-latency matching engine + simulated exchange"
    LANGUAGES CXX)

# CMake 3.24+ should timestamp extracted URL content at extraction time so a
# changed archive reliably rebuilds dependants. Older supported CMake versions
# do not define this policy and retain their native behavior.
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(cmake/Dependencies.cmake)

option(BABO_BUILD_CLIENT "Build the native terminal client" ON)

babo_fetch_exchange_dependencies()
if(BABO_BUILD_CLIENT)
    babo_fetch_client_dependencies()
endif()

# ---- babobook (the matching core) ------------------------------------------
# Vendored copy of the matching-engine library. Multiplatform, network-agnostic,
# header-only + one .cpp; exposes babo::book::matching_book. This is the core we
# wrap; the exchange is the system around it.
add_subdirectory(libs/babobook)
add_subdirectory(third_party/rigtorp/MPMCQueue)
add_subdirectory(third_party/rigtorp/SPSCQueue)

add_subdirectory(src)
if(BABO_BUILD_CLIENT)
    add_subdirectory(client)
endif()
