# tlog_timing: platform-free timing-gate logic (design D2): frame header and bodies, advert and
# read-stream codecs, crossing detector, run state machine, BREAK handling, advert pages, passage
# queue. ESP-IDF component on the device, static library on the host.
set(TLOG_TIMING_SRCS src/link.c src/timing.c src/crossing.c src/run.c src/pqueue.c src/brk.c src/pages.c src/crc32.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${TLOG_TIMING_SRCS} INCLUDE_DIRS include)
else()
    cmake_minimum_required(VERSION 3.16)
    list(TRANSFORM TLOG_TIMING_SRCS PREPEND ${CMAKE_CURRENT_LIST_DIR}/)
    add_library(tlog_timing STATIC ${TLOG_TIMING_SRCS})
    target_include_directories(tlog_timing PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_link_libraries(tlog_timing PUBLIC m)
    set_target_properties(tlog_timing PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    target_compile_options(tlog_timing PRIVATE -Wall -Wextra -Werror)
endif()
