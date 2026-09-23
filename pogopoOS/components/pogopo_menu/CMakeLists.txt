idf_component_register(
    SRCS
        "src/menu_assets.cpp"
        "src/pogo_font.cpp"
    INCLUDE_DIRS "include"
    REQUIRES pogopo_graphics
    EMBED_FILES "menu_assets.bin"
)

file(SIZE "${CMAKE_CURRENT_LIST_DIR}/menu_assets.bin" POGOPO_MENU_ASSETS_SIZE)
if(NOT POGOPO_MENU_ASSETS_SIZE EQUAL 745556)
    message(FATAL_ERROR
        "pogopo menu_assets.bin is ${POGOPO_MENU_ASSETS_SIZE} bytes; expected 745556")
endif()

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
