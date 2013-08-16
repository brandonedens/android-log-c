# Make file for android logger software.
# Copyright 2013 Brandon Edens. All Rights Reserved
# Brandon Edens <brandonedens@gmail.com>
# 2013-08-14
cmake_minimum_required(VERSION 2.8)

project(android-log)

include(CheckIncludeFiles)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/config.h)


add_definitions("-std=gnu11 -Og -g -fbuiltin -Wall -ggdb -Wshadow")

include_directories(-isystem "../lib/ccan" ${CMAKE_CURRENT_BINARY_DIR})

add_executable(android-log
    main.c
    ../lib/ccan/ccan/strmap/strmap.c
    ../lib/ccan/ccan/ilog/ilog.c
    )

target_link_libraries(android-log
    pthread
    )

