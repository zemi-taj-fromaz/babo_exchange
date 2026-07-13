add_executable(babo_exchange
    main.cpp
    core/logging.cpp
    core/main_process.cpp
    feed/bitstamp.cpp
    feed/bitstamp_ws.cpp)

target_compile_features(babo_exchange PRIVATE cxx_std_20)

# Headers are included as "core/logging.hpp", i.e. relative to src/.
target_include_directories(babo_exchange PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(babo_exchange PRIVATE
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    SPSCQueue::SPSCQueue
    babobook
    ixwebsocket)

# On MinGW-style toolchains (llvm-mingw / CLion), statically link the C++ runtime
# so the exe runs standalone (no libc++/libunwind/winpthread DLLs to ship).
if(MINGW)
    target_link_options(babo_exchange PRIVATE -static)
endif()
