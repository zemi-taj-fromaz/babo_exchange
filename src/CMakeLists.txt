add_executable(babo_exchange
    main.cpp
    core/logging.cpp
    core/main_process.cpp
    egress/client_order_listener.cpp
    egress/latest_depth_mailbox.cpp
    feed/bitstamp.cpp
    feed/bitstamp_ws.cpp)

target_compile_features(babo_exchange PRIVATE cxx_std_20)

# Headers are included as "core/logging.hpp", i.e. relative to src/.
target_include_directories(babo_exchange PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(babo_exchange PRIVATE
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    MPMCQueue::MPMCQueue
    SPSCQueue::SPSCQueue
    babobook
    ixwebsocket)

# Apple currently ships the C++20 stop-token implementation behind libc++'s
# experimental-library feature gate. Keep using the standard jthread/stop_token
# API and enable the corresponding headers + runtime only for AppleClang.
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    target_compile_options(babo_exchange PRIVATE -fexperimental-library)
    target_link_options(babo_exchange PRIVATE -fexperimental-library)
endif()

# On MinGW-style toolchains (llvm-mingw / CLion), statically link the C++ runtime
# so the exe runs standalone (no libc++/libunwind/winpthread DLLs to ship).
if(MINGW)
    target_link_options(babo_exchange PRIVATE -static)
endif()
