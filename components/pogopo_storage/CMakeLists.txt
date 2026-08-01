idf_component_register(
    SRCS "src/storage.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_sdmmc sdmmc fatfs
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

