zephyr_library()

if (CONFIG_ZMK_CENTRAL_STATES_RELAY)

  if (CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zephyr_library_sources(src/central_states_sender.c)
  endif()

  if (CONFIG_ZMK_SPLIT AND (NOT CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
    zephyr_library_sources(src/central_states_receiver.c)
  endif()

  zephyr_include_directories(include)
  zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)

endif()
