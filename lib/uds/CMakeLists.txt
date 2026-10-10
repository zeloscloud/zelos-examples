zephyr_library()
zephyr_library_sources(core/uds_core.c)
zephyr_library_sources_ifdef(CONFIG_ZELOS_UDS zephyr/uds.c)
