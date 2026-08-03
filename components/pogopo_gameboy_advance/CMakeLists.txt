set(GPSP_DIR "third_party/gpsp")
set(GPSP_COMMON "${GPSP_DIR}/libretro/libretro-common")

idf_component_register(
    SRCS
        "src/gameboy_advance.cpp"
        "${GPSP_DIR}/cpu.cpp"
        "${GPSP_DIR}/video.cpp"
        "${GPSP_DIR}/main.c"
        "${GPSP_DIR}/gba_memory.c"
        "${GPSP_DIR}/savestate.c"
        "${GPSP_DIR}/input.c"
        "${GPSP_DIR}/sound.c"
        "${GPSP_DIR}/cheats.c"
        "${GPSP_DIR}/memmap.c"
        "${GPSP_DIR}/serial.c"
        "${GPSP_DIR}/gbp.c"
        "${GPSP_DIR}/rfu.c"
        "${GPSP_DIR}/gba_cc_lut.c"
    INCLUDE_DIRS
        "include"
        "${GPSP_DIR}"
        "${GPSP_DIR}/libretro"
        "${GPSP_COMMON}/include"
    REQUIRES
        pogopo_audio
        pogopo_graphics
        esp_timer
        heap
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O3
    -fomit-frame-pointer
    -ffast-math
    -fno-strict-aliasing
    -DRETRO_GO
    -DPOGOPO_GBA
    -DOVERCLOCK_60FPS
    -DROM_BUFFER_SIZE=6
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-implicit-fallthrough
    -Wno-missing-field-initializers
    -Wno-unused-label
    -Wno-sign-compare
)
