cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bench_dcdc)

target_sources(app PRIVATE src/main.c)
# Measurement and calibration over XCP, only with `-S xcp`. The build also
# writes zephyr/dcdc.a2l, dcdc.a2l with this build's addresses.
if(CONFIG_ZELOS_XCP)
  target_sources(app PRIVATE src/xcp.c)
  zelos_xcp_a2l(dcdc.a2l)
endif()
