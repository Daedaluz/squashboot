cmake_minimum_required(VERSION 3.22)
project(squashboot C)

set(CMAKE_C_STANDARD 11)

set(NOLIBC_DIR ${CMAKE_SOURCE_DIR}/third_party/nolibc)

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
        sys.c
        dir.c
)

target_include_directories(squashboot PRIVATE ${NOLIBC_DIR})

# No libc: freestanding build against vendored nolibc (third_party/nolibc).
# -static is implied since there is nothing dynamic left to link against.
target_compile_options(squashboot PRIVATE
    -nostdlib
    -ffreestanding
    -fno-stack-protector
    -fno-asynchronous-unwind-tables
    -fno-ident
)

target_link_options(squashboot PRIVATE
    -nostdlib
    -static
    -no-pie
)

# libgcc provides compiler runtime helpers (e.g. 64-bit division on some
# archs); it is part of the toolchain, not libc.
target_link_libraries(squashboot PRIVATE gcc)

target_compile_definitions(squashboot PRIVATE
    GIT_VERSION="${GIT_VERSION}"
    GIT_COMMIT="${GIT_COMMIT}"
)

# Not part of `all` — fetches from the network. Run explicitly with:
#   cmake --build build --target update-nolibc
# Updates to the latest stable kernel tag. To pin a specific ref instead,
# run scripts/update-nolibc.sh <ref> directly.
add_custom_target(update-nolibc
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/update-nolibc.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Updating vendored third_party/nolibc from torvalds/linux"
    USES_TERMINAL
)
