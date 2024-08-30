cmake_minimum_required(VERSION 3.22)
project(squashboot C)

set(CMAKE_C_STANDARD 11)
link_libraries("-static")
add_executable(squashboot
        main.c
        util.c
        util.h)
