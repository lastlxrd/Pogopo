idf_component_register(
    SRCS
        "src/drivers/sharp_display.cpp"
        "src/canvas/canvas.cpp"
        "src/font/font5x7.cpp"
        "src/core/graphics.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_spi
        esp_driver_gpio
        esp_timer
        freertos
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
