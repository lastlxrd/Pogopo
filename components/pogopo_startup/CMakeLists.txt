idf_component_register(
    SRCS "src/startup_animation.cpp"
    INCLUDE_DIRS "include"
    REQUIRES pogopo_graphics
    EMBED_FILES "assets/startup_frames.bin"
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
