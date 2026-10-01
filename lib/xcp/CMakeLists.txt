zephyr_library()
zephyr_library_sources(core/xcp_core.c)
zephyr_library_sources_ifdef(CONFIG_ZELOS_XCP zephyr/xcp.c)
