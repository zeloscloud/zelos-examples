zephyr_library()
zephyr_library_sources(canopen.c)

# The stack's module comes after this one, so its library cannot be named here:
# every C file of a CANopen build gets the TIME types.
zephyr_compile_options($<$<COMPILE_LANGUAGE:C>:-include${CMAKE_CURRENT_SOURCE_DIR}/co_time_types.h>)
