idf_component_register(
    SRCS
        "src/app_manager.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        pogopo_graphics
        pogopo_input
        pogopo_haptics
        pogopo_audio
        pogopo_gui
        pogopo_storage
        pogopo_imu
        pogopo_power
        pogopo_settings
        pogopo_menu
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
