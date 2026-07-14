add_library(MPMCQueue INTERFACE)
add_library(MPMCQueue::MPMCQueue ALIAS MPMCQueue)

target_include_directories(MPMCQueue INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(MPMCQueue INTERFACE cxx_std_20)
