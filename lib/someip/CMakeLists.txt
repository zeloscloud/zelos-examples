zephyr_library()
zephyr_library_sources(core/someip_core.c)
zephyr_library_sources_ifdef(CONFIG_ZELOS_SOMEIP zephyr/someip.c)
