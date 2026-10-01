zephyr_library()
zephyr_library_sources(core/xcp_core.c)
zephyr_library_sources_ifdef(CONFIG_ZELOS_XCP zephyr/xcp.c)

# Write zephyr/<name>.a2l from the A2L template <name>.a2l, with this build's
# addresses. Rebuilt whenever the ELF or the template changes.
function(zelos_xcp_a2l template)
  get_filename_component(template ${template} ABSOLUTE)
  get_filename_component(name ${template} NAME_WE)
  set(a2l ${ZEPHYR_BINARY_DIR}/${name}.a2l)
  set(tool ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../tools/a2l_addresses.py)
  add_custom_command(
    OUTPUT ${a2l}
    COMMAND ${PYTHON_EXECUTABLE} ${tool} ${template}
            $<TARGET_FILE:${logical_target_for_zephyr_elf}> ${a2l}
    DEPENDS ${template} ${tool} ${logical_target_for_zephyr_elf}
  )
  add_custom_target(${name}_a2l ALL DEPENDS ${a2l})
endfunction()
