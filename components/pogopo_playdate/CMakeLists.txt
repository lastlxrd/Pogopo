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

# Keep the upstream Lua files readable in the archive while compiling an exact
# byte-for-byte copy into flash. This generated source lives only in the build
# directory and is recreated whenever a game Lua file changes.
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
idf_component_register(
    SRCS
        "src/runtime.cpp"
        "${PDSNAKE_GENERATED}"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        lua
        pogopo_graphics
        pogopo_audio
        pogopo_storage
        esp_timer
        heap
)
set_source_files_properties("${PDSNAKE_GENERATED}" PROPERTIES GENERATED TRUE)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
