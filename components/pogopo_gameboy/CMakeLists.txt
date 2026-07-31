idf_component_register(
    SRCS
        "src/gameboy.cpp"
        "third_party/minigb_apu.c"
    INCLUDE_DIRS
        "include"
        "third_party"
    REQUIRES
        pogopo_audio
        pogopo_graphics
        esp_timer
        heap
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
# Peanut-GB and MiniGB APU are the dominant hot path. Keep the rest of the OS at
# the project-wide performance setting, but let this component use full -O3.
target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
