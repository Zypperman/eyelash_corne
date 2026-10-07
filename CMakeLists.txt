# Custom code for this config repo (built in because zephyr/module.yml
# points here). See src/layer_rgb.c.

if(CONFIG_ZMK_LAYER_RGB)
  zephyr_library()
  zephyr_library_sources(src/layer_rgb.c)
  zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()
