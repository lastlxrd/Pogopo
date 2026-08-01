idf_component_register(
    SRCS
        "app_main.cpp"
        "apps/demo_apps.cpp"
        "apps/gameboy_apps.cpp"
        "i2c_bus.cpp"
        "peripheral_tests.cpp"
        "tasks.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_i2c
        esp_driver_gpio
        pogopo_graphics
        pogopo_input
        pogopo_haptics
        pogopo_audio
        pogopo_storage
        pogopo_imu
        pogopo_power
        pogopo_settings
        pogopo_gameboy
        pogopo_gui
        pogopo_app
        esp_timer
        esp_system
        spi_flash
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

