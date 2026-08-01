idf_component_register(
    SRCS
        "src/haptics.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_driver_gpio
        freertos
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

