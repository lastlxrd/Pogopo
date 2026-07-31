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
# The interpreter is faster at the project-wide performance level without the
# code-size and instruction-cache regression observed with STEP9.3's -O3.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
