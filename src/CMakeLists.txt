zephyr_library()

# Same as drivers/input: ZMK's full internal headers (zmk/keymap.h,
# zmk/event_manager.h, zmk/events/*) aren't exposed to external modules
# by default.
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_ZMK_ENDPOINT_LAYER endpoint_layer.c)
