idf_component_register(
    SRCS "src/startup_animation.cpp"
    INCLUDE_DIRS "include"
    REQUIRES pogopo_graphics
    # Keep the embedded blob at the component root. Some ESP-IDF/Windows
    # combinations flatten subdirectory paths when naming binary symbols,
    # which made the former assets/startup_frames.bin reference disagree with
    # the symbol expected by startup_animation.cpp.
    EMBED_FILES "startup_frames.bin"
)

file(SIZE "${CMAKE_CURRENT_LIST_DIR}/startup_frames.bin" STARTUP_FRAMES_SIZE)
if(NOT STARTUP_FRAMES_SIZE EQUAL 1332000)
    message(FATAL_ERROR
        "pogopo startup_frames.bin is ${STARTUP_FRAMES_SIZE} bytes; expected 1332000")
endif()

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
