idf_component_register(
    SRCS "src/imu.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bmi270 esp_driver_i2c
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

