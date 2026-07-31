idf_component_register(
    SRCS "pogopo_graphics.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_spi
        esp_driver_gpio
        esp_timer
        freertos
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
