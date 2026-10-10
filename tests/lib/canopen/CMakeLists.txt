cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(canopen_test)

# The PDU's object dictionary, so the stack runs as on the node.
zephyr_include_directories(../../../nodes/pdu/od)
target_sources(app PRIVATE src/main.c ../../../nodes/pdu/od/CO_OD.c)
