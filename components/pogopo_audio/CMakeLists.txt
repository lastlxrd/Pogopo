idf_component_register(
    SRCS
        "src/audio.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_driver_i2s
        esp_timer
        freertos
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
