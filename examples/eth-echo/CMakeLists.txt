cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(eth_echo)

target_sources(app PRIVATE main.c src/bwtest.c src/bench_shell.c)
