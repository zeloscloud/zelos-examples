cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(j1939_test)

# The sans-IO core alone: no CAN controller needed.
target_sources(app PRIVATE src/main.c ../../../lib/j1939/core/j1939.c)
