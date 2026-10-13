cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(motor)

target_sources(app PRIVATE src/main.c src/control.c src/tune.c src/health.c src/settings.c)
