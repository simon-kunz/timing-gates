# Host build of the platform-free timing component and its Unity tests.
cmake_minimum_required(VERSION 3.16)
project(timing_host C)
enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

include(FetchContent)
FetchContent_Declare(unity GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git GIT_TAG v2.6.0)
FetchContent_Populate(unity)
add_library(unity STATIC ${unity_SOURCE_DIR}/src/unity.c)
target_include_directories(unity PUBLIC ${unity_SOURCE_DIR}/src)
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE UNITY_SUPPORT_64)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../components/tlog_timing ${CMAKE_BINARY_DIR}/tlog_timing)

foreach(t timing crossing run brk pages pqueue)
    add_executable(test_${t} test_${t}.c)
    target_link_libraries(test_${t} PRIVATE tlog_timing unity m)
    target_compile_options(test_${t} PRIVATE -Wall -Wextra -Werror)
    add_test(NAME ${t} COMMAND test_${t})
endforeach()
