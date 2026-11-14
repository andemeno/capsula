cmake_minimum_required(VERSION 3.16)
project(capsula LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_compile_options(-Wall -Wextra -Wpedantic -fno-exceptions)

add_library(capsula_core STATIC
    include/capsula/protocol.hpp
    include/capsula/util.hpp
    include/capsula/child.hpp

    src/protocol.cpp
    src/util.cpp
    src/child.cpp
)
target_include_directories(capsula_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(capsula_core PUBLIC _GNU_SOURCE)

add_executable(capsula
    include/capsula/client.hpp
    include/capsula/daemon.hpp

    src/main.cpp
    src/client.cpp
    src/daemon.cpp
)
target_link_libraries(capsula PRIVATE capsula_core)

enable_testing()
add_test(NAME container_integration
    COMMAND ${CMAKE_SOURCE_DIR}/tests/test_container.sh
            $<TARGET_FILE:capsula> ${CMAKE_SOURCE_DIR})
set_tests_properties(container_integration PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 240)

add_test(NAME daemon_protocol
    COMMAND ${CMAKE_SOURCE_DIR}/tests/test_daemon.sh
            $<TARGET_FILE:capsula> ${CMAKE_SOURCE_DIR})
set_tests_properties(daemon_protocol PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 240)
