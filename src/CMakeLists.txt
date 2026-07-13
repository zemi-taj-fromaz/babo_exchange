add_executable(babo_exchange
    main.cpp
    core/logging.cpp)

target_compile_features(babo_exchange PRIVATE cxx_std_20)

# Headers are included as "core/logging.hpp", i.e. relative to src/.
target_include_directories(babo_exchange PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(babo_exchange PRIVATE spdlog::spdlog)
