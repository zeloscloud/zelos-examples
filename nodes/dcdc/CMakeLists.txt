cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bench_dcdc)

target_sources(app PRIVATE src/main.c)
# Measurement and calibration over XCP, only with `-S xcp`.
target_sources_ifdef(CONFIG_ZELOS_XCP app PRIVATE src/xcp.c)
