idf_component_register(
    SRCS
        "src/input.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_driver_i2c
        esp_driver_gpio
        freertos
        esp_timer
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

