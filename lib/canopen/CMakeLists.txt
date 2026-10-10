zephyr_library()
zephyr_library_sources(canopen.c CO_driver.c)

# CO_driver.c here replaces the module's src/CO_driver.c, which stops, re-times
# and restarts the controller on every CO_init; lib/can owns it instead.
# zephyr/module.yml orders the module first, so its directory is known here.
set_source_files_properties(${ZEPHYR_CANOPENNODEZEPHYR_MODULE_DIR}/src/CO_driver.c
  DIRECTORY ${ZEPHYR_CANOPENNODEZEPHYR_MODULE_DIR}
  PROPERTIES HEADER_FILE_ONLY ON)

# Every C file of a CANopen build gets the TIME types: the stack's library has
# no stable name to scope them to.
zephyr_compile_options($<$<COMPILE_LANGUAGE:C>:-include${CMAKE_CURRENT_SOURCE_DIR}/co_time_types.h>)
