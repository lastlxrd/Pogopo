idf_component_register(
    SRCS
        "src/app_manager.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        pogopo_graphics
        pogopo_input
        pogopo_haptics
        pogopo_gui
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
