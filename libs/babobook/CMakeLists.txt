# --- babo_book -------------------------------------------------------------
# Your own order book + matching algorithm. Currently a stub (see src) -- this
# is the library you'll be writing.
add_library(babobook STATIC
        src/simple_order.cpp
)

target_include_directories(babobook PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(babobook PUBLIC cxx_std_20)
