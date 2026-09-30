# This repository is a Zephyr module.
#
# It contributes the CAN helper and the codec generated from dbc/bench.dbc to
# every application under apps/. Both are built once and linked by all three
# nodes, so the wire format has one implementation.

zephyr_include_directories(include codec)

zephyr_library()
zephyr_library_sources(
  lib/bench_can.c
  codec/bench.c
)
