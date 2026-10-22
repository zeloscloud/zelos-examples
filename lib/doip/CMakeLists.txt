zephyr_library()
zephyr_library_sources(core/doip_core.c)
zephyr_library_sources_ifdef(CONFIG_ZELOS_DOIP zephyr/doip.c)
