set(PDSNAKE_LUA_FILES
    "fonts.lua"
    "icons.lua"
    "layout.lua"
    "main.lua"
    "meta.lua"
    "scenes.lua"
    "screen.lua"
    "settings.lua"
    "sfx.lua"
    "stats.lua"
    "store.lua"
    "scenes/gameover.lua"
    "scenes/gameplay.lua"
    "scenes/main_menu.lua"
    "scenes/more.lua"
    "scenes/stats.lua"
)

set(CELESTE_LUA_FILES
    "main.lua"
    "Scripts/Game.lua"
    "Scripts/Options.lua"
    "Scripts/celeste.lua"
    "Scripts/data.lua"
    "Scripts/globals.lua"
    "Scripts/pico-8.lua"
)

# Keep both upstream games readable in the archive while compiling exact
# copies into flash. The generated translation units live only in build/.
set(PDSNAKE_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/pdsnake_sources.cpp")
file(WRITE "${PDSNAKE_GENERATED}"
    "#include <cstddef>\n#include \"embedded_source.h\"\n\nnamespace pogopo::playdate {\n")
set(PDSNAKE_TABLE_ROWS "")
set(PDSNAKE_INDEX 0)
foreach(PDSNAKE_FILE IN LISTS PDSNAKE_LUA_FILES)
    set(PDSNAKE_PATH "${CMAKE_CURRENT_LIST_DIR}/game/pdsnake/source/${PDSNAKE_FILE}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PDSNAKE_PATH}")
    file(READ "${PDSNAKE_PATH}" PDSNAKE_TEXT)
    file(APPEND "${PDSNAKE_GENERATED}"
        "static const char source_${PDSNAKE_INDEX}[] = R\"PDSNAKE_SOURCE(${PDSNAKE_TEXT})PDSNAKE_SOURCE\";\n")
    string(APPEND PDSNAKE_TABLE_ROWS
        "    {\"${PDSNAKE_FILE}\", source_${PDSNAKE_INDEX}, sizeof(source_${PDSNAKE_INDEX}) - 1U},\n")
    math(EXPR PDSNAKE_INDEX "${PDSNAKE_INDEX} + 1")
endforeach()
file(APPEND "${PDSNAKE_GENERATED}"
    "\nstatic const EmbeddedSource sources[] = {\n${PDSNAKE_TABLE_ROWS}};\n\n"
    "const EmbeddedSource* pdsnakeSources(size_t& count) {\n"
    "    count = sizeof(sources) / sizeof(sources[0]);\n"
    "    return sources;\n}\n\n} // namespace pogopo::playdate\n")

set(CELESTE_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/celeste_sources.cpp")
file(WRITE "${CELESTE_GENERATED}"
    "#include <cstddef>\n#include \"embedded_source.h\"\n\nnamespace pogopo::playdate {\n")
set(CELESTE_TABLE_ROWS "")
set(CELESTE_INDEX 0)
foreach(CELESTE_FILE IN LISTS CELESTE_LUA_FILES)
    set(CELESTE_PATH "${CMAKE_CURRENT_LIST_DIR}/game/celeste/source/${CELESTE_FILE}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CELESTE_PATH}")
    file(READ "${CELESTE_PATH}" CELESTE_TEXT)
    file(APPEND "${CELESTE_GENERATED}"
        "static const char source_${CELESTE_INDEX}[] = R\"CELESTE_SOURCE(${CELESTE_TEXT})CELESTE_SOURCE\";\n")
    string(APPEND CELESTE_TABLE_ROWS
        "    {\"${CELESTE_FILE}\", source_${CELESTE_INDEX}, sizeof(source_${CELESTE_INDEX}) - 1U},\n")
    math(EXPR CELESTE_INDEX "${CELESTE_INDEX} + 1")
endforeach()
file(APPEND "${CELESTE_GENERATED}"
    "\nstatic const EmbeddedSource sources[] = {\n${CELESTE_TABLE_ROWS}};\n\n"
    "const EmbeddedSource* celesteSources(size_t& count) {\n"
    "    count = sizeof(sources) / sizeof(sources[0]);\n"
    "    return sources;\n}\n\n} // namespace pogopo::playdate\n")

set(COMPAT_PATH "${CMAKE_CURRENT_LIST_DIR}/src/compat.lua")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${COMPAT_PATH}")
file(READ "${COMPAT_PATH}" COMPAT_TEXT)
set(COMPAT_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/compat_source.cpp")
file(WRITE "${COMPAT_GENERATED}"
    "#include <cstddef>\n#include \"embedded_source.h\"\n\nnamespace pogopo::playdate {\n"
    "static const char source[] = R\"COMPAT_SOURCE(${COMPAT_TEXT})COMPAT_SOURCE\";\n"
    "const char* compatSource(size_t& size) { size = sizeof(source) - 1U; return source; }\n"
    "} // namespace pogopo::playdate\n")

idf_component_register(
    SRCS
        "src/runtime_celeste.cpp"
        "src/pdx_package.cpp"
        "src/celeste_assets.cpp"
        "${PDSNAKE_GENERATED}"
        "${CELESTE_GENERATED}"
        "${COMPAT_GENERATED}"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        lua
        pogopo_graphics
        pogopo_menu
        pogopo_audio
        pogopo_storage
        esp_timer
        heap
        zlib
)

# These commands intentionally remain after idf_component_register(): ESP-IDF
# evaluates an early script-mode dependency pass where this command is illegal.
set_source_files_properties(
    "${PDSNAKE_GENERATED}"
    "${CELESTE_GENERATED}"
    "${COMPAT_GENERATED}"
    PROPERTIES GENERATED TRUE
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
# The runtime's image, collision and bytecode-normalization loops dominate
# Playdate frame time on ESP32-S3. Keep this component on the performance
# profile even when the rest of the firmware uses the balanced default.
target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
