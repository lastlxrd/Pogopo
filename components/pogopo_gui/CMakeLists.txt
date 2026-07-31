idf_component_register(
    SRCS
        "src/gui.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        pogopo_graphics
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
