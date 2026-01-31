cmake_minimum_required(VERSION 3.22)
project(squashboot C)

set(CMAKE_C_STANDARD 11)
link_libraries("-static")

# Get git version information
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_VERSION)
    set(GIT_VERSION "unknown")
endif()

execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_COMMIT)
    set(GIT_COMMIT "unknown")
endif()

add_executable(squashboot
        main.c
)

target_compile_definitions(squashboot PRIVATE
    GIT_VERSION="${GIT_VERSION}"
    GIT_COMMIT="${GIT_COMMIT}"
)
