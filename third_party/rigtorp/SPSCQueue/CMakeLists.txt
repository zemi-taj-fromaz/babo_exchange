add_library(SPSCQueue INTERFACE)
add_library(SPSCQueue::SPSCQueue ALIAS SPSCQueue)

target_include_directories(SPSCQueue INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(SPSCQueue INTERFACE cxx_std_11)
