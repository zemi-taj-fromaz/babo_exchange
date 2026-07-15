add_executable(babo_client
    main.cpp
    gateway_connection.cpp)

target_compile_features(babo_client PRIVATE cxx_std_20)
target_include_directories(babo_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(babo_client PRIVATE
    ftxui::screen
    ftxui::dom
    ftxui::component)

if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    target_compile_options(babo_client PRIVATE -fexperimental-library)
    target_link_options(babo_client PRIVATE -fexperimental-library)
endif()

if(MINGW)
    target_link_options(babo_client PRIVATE -static)
endif()

if(WIN32)
    target_link_libraries(babo_client PRIVATE ws2_32)
endif()
