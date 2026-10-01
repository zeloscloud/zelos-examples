cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zelos_pdu)

# od/pdu.eds is the object dictionary; od/CO_OD.c and od/CO_OD.h are generated
# from it by `just canopen-od pdu`, so a master reading the EDS and the
# firmware cannot disagree. The stack's own sources include CO_OD.h, hence a
# global include directory.
zephyr_include_directories(od)
target_sources(app PRIVATE src/main.c od/CO_OD.c)
