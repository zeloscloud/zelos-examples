zephyr_library()
zephyr_library_sources(core/goose_core.c)
zephyr_library_sources_ifdef(CONFIG_ZELOS_GOOSE zephyr/goose.c)
