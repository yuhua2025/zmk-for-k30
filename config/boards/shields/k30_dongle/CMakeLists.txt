if(CONFIG_ZMK_DISPLAY)
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
  zephyr_library_sources(k30_dongle_status_screen.c)
endif()
