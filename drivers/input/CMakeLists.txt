zephyr_library()

# ZMK's own internal headers (zmk/hid.h, zmk/keymap.h, zmk/endpoints.h,
# dt-bindings/zmk/hid_usage*.h) live under the app's include/ dir, which
# isn't exposed to external modules by default - only a small curated
# subset under app/module/include/ is. analog_joystick.c needs the full
# set to switch between mouse-move and arrow-key output, so add it
# explicitly for this library. APPLICATION_SOURCE_DIR is Zephyr's
# standard CMake variable for the app being built (zmk/app here).
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_INPUT_ANALOG_JOYSTICK analog_joystick.c)
