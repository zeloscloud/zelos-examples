cmake_minimum_required(VERSION 3.20.0)
# XCP, UDS, AUTOSAR and Ethernet are part of this image, so their snippets apply without -S.
set(SNIPPET xcp uds autosar ethernet CACHE STRING "Snippets every build of this image takes")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(full)

# One MCU, three logical ECUs, from the single-node apps' own sources: the
# DC-DC with XCP, UDS and AUTOSAR PDUs, the genset (J1939) and the PDU (CANopen).
set(nodes ${CMAKE_CURRENT_SOURCE_DIR}/..)
zephyr_include_directories(${nodes}/pdu/od)
target_sources(app PRIVATE
  src/main.c src/waveform.c
  ${nodes}/dcdc/src/dcdc.c ${nodes}/dcdc/src/xcp.c ${nodes}/dcdc/src/uds.c
  ${nodes}/dcdc/src/autosar.c ${nodes}/dcdc/src/autosar_pdus.c
  ${nodes}/genset/src/genset.c
  ${nodes}/pdu/src/pdu.c ${nodes}/pdu/od/CO_OD.c
)
target_sources_ifdef(CONFIG_ZELOS_SOMEIP app PRIVATE ${nodes}/dcdc/src/someip.c)
# zephyr/dcdc.a2l, the DC-DC's A2L with this image's addresses.
zelos_xcp_a2l(${nodes}/dcdc/dcdc.a2l)
