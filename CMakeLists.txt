# Native module graph, derived from the pinned webmscore module selection.
# Production builds use this file directly; they do not rewrite upstream CMake or web/main.cpp.
cmake_minimum_required(VERSION 3.22)

project(musescore_mcp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.upstream/webmscore" CACHE PATH "Pinned MuseScore source checkout")
set(NATIVE_HOST "${CMAKE_CURRENT_SOURCE_DIR}")
set(PROJECT_ROOT_DIR ${MU_ROOT})

set(THIRDPARTY_DIR ${MU_ROOT}/thirdparty)
set(MUSE_FRAMEWORK_PATH ${MU_ROOT})
set(MUSE_FRAMEWORK_SRC_PATH ${MU_ROOT}/src/framework)

set(CMAKE_MODULE_PATH
    ${MU_ROOT}
    ${MU_ROOT}/buildscripts
    ${MU_ROOT}/buildscripts/cmake
    ${MUSE_FRAMEWORK_SRC_PATH}/cmake
    ${CMAKE_MODULE_PATH}
    )

# Load MuseScore version
include(version)

include(GetPlatformInfo)
include(GetCompilerInfo)
include(SetupCompileWarnings)
include(SetupCompilerCache)

###########################################
# Configuration
###########################################

# Declares the MUSE_* options with their default values
include(MuseDeclareOptions)

set(MUSE_APP_REVISION "")
set(MUSE_APP_BUILD_NUMBER "${CMAKE_BUILD_NUMBER}")
set(MUSE_APP_INSTALL_PREFIX "\"${CMAKE_INSTALL_PREFIX}\"")
set(MUSE_APP_INSTALL_NAME "\"\"")

set(MUSE_QT_SUPPORT OFF)
set(QT_SUPPORT OFF)

# webmscore is a single threaded library: the audio engine runs in-process,
# on the same thread as everything else (see MUSE_AUDIO_INLINE_ENGINE)
set(MUSE_THREADS_SUPPORT OFF)
set(MUSE_CONFIGURATION_IS_WEB OFF)
set(MUSE_WEB_LIB OFF)
set(MUSE_MODULE_AUDIO_WORKMODE 1)

set(MUSE_ENABLE_UNIT_TESTS OFF)
set(MUSE_COMPILE_USE_PCH OFF)
set(MUSE_COMPILE_USE_UNITY OFF)
set(MUSE_COMPILE_USE_COMPILER_CACHE ON)

set(QT_ADD_CONCURRENT OFF)
set(QT_ADD_LINGUISTTOOLS OFF)
set(QT_ADD_WEBSOCKET OFF)
set(QT_QPROCESS_SUPPORTED OFF)
set(QT_CONCURRENT_SUPPORTED OFF)

# framework modules
set(MUSE_MODULE_ACCESSIBILITY OFF)
set(MUSE_MODULE_ACTIONS ON)
set(MUSE_MODULE_AUDIO ON)
set(MUSE_MODULE_AUDIO_EXPORT ON)
set(MUSE_MODULE_AUDIO_JACK OFF)
set(MUSE_MODULE_AUDIOPLUGINS OFF)
set(MUSE_MODULE_AUTOBOT OFF)
set(MUSE_MODULE_CLOUD OFF)
set(MUSE_MODULE_DIAGNOSTICS OFF)
set(MUSE_MODULE_DIAGNOSTICS_CRASHPAD_CLIENT OFF)
set(MUSE_MODULE_DOCKWINDOW OFF)
set(MUSE_MODULE_DRAW ON)
set(MUSE_MODULE_EXTENSIONS OFF)
set(MUSE_MODULE_GLOBAL ON)
set(MUSE_MODULE_LANGUAGES OFF)
set(MUSE_MODULE_LEARN OFF)
set(MUSE_MODULE_MEDIA OFF)
set(MUSE_MODULE_MIDI OFF)
set(MUSE_MODULE_MIDIREMOTE OFF)
set(MUSE_MODULE_MPE ON)
set(MUSE_MODULE_MULTIWINDOWS OFF)
set(MUSE_MODULE_MUSESAMPLER OFF)
set(MUSE_MODULE_NETWORK OFF)
set(MUSE_MODULE_SHORTCUTS OFF)
set(MUSE_MODULE_TOURS OFF)
set(MUSE_MODULE_UI OFF)
set(MUSE_MODULE_UI_QML OFF)
set(MUSE_MODULE_UPDATE OFF)
set(MUSE_MODULE_VST OFF)
set(MUSE_MODULE_WORKSPACE OFF)

# MuseScore modules
set(MUE_BUILD_APPSHELL_MODULE OFF)
set(MUE_BUILD_BRAILLE_MODULE OFF)
set(MUE_BUILD_CONVERTER_MODULE OFF)
set(MUE_BUILD_INSPECTOR_MODULE OFF)
set(MUE_BUILD_INSTRUMENTSSCENE_MODULE OFF)
set(MUE_BUILD_MUSESOUNDS_MODULE OFF)
set(MUE_BUILD_NOTATION_MODULE OFF)
set(MUE_BUILD_NOTATIONSCENE_MODULE OFF)
set(MUE_BUILD_PALETTE_MODULE OFF)
set(MUE_BUILD_PLAYBACK_MODULE OFF)
set(MUE_BUILD_PREFERENCES_MODULE OFF)
set(MUE_BUILD_PRINT_MODULE OFF)
set(MUE_BUILD_PROJECT_MODULE OFF)
set(MUE_BUILD_ENGRAVING_QML OFF)
set(MUE_BUILD_ENGRAVING_TESTS OFF)
set(MUE_BUILD_ENGRAVING_DEVTOOLS OFF)
set(MUE_BUILD_ENGRAVING_PLAYBACK ON)
set(MUE_BUILD_IMPORTEXPORT_TESTS OFF)
set(MUE_DOWNLOAD_SOUNDFONT OFF)
set(MUE_INSTALL_SOUNDFONT OFF)
set(MUE_RUN_LRELEASE OFF)

# Generates muse_framework_config.h and includes MuseCreateModule
set(MUSE_MODULE_AUDIO_ASIO OFF)
set(MUSE_MODULE_DRAW_USE_QTFONTMETRICS OFF CACHE BOOL "" FORCE)
include(MuseSetupConfiguration)

add_definitions(
    -DENGRAVING_NO_QTEXTDOCUMENT
    -DENGRAVING_NO_INTERACTIVE
    -DDRAW_NO_QSVGRENDER           # QSvgRenderer (QtSvg) depends on QWidget in Qt5, this changes in Qt6
    -DMUSE_MODULE_AUDIO_EXPORT
    -DMUE_BUILD_ENGRAVING_PLAYBACK
    -DMUSE_AUDIO_INLINE_ENGINE     # the audio engine runs in-process, on the main thread
    -DCUSTOM_ALLOCATOR_DISABLED    # the custom memory allocator malfunctions ('memory access out of bounds') when destroying and re-creating a new `WebMscore` instance, so disable it

    -DMUSICXML_NO_VALIDATION
    -DMUSICXML_NO_INTERACTIVE
)

include_directories(
    ${MU_ROOT}/web
    ${MU_ROOT}/thirdparty/dtl
)


set(BUILD_SHARED_LIBS OFF)
set(MUSE_AUDIO_INLINE_ENGINE ON)
set(MUSE_MODULE_AUDIO_ASIO OFF)
find_package(ZLIB REQUIRED)
# The selected upstream modules share ZIP/font compression helpers. Keep this
# directory-level dependency so their legacy CMake files receive its includes.
link_libraries(ZLIB::ZLIB)
if(MSVC)
    add_compile_options(/utf-8 /bigobj /Zc:__cplusplus)
    add_compile_definitions(NOMINMAX _USE_MATH_DEFINES UNICODE _UNICODE)
endif()


###########################################
# Setup external dependencies
###########################################



include(SetupFreetypeBrotli)

###########################################
# Add source tree
###########################################


set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(GLOBAL_NO_INTERNAL ON)
set(DRAW_NO_INTERNAL ON)
set(ENGRAVING_NO_ACCESSIBILITY ON)
set(ENGRAVING_NO_API ON)
add_subdirectory(${MU_ROOT}/src/framework/global global)
add_subdirectory(${MU_ROOT}/src/framework/draw draw)
add_subdirectory(${MU_ROOT}/src/framework/mpe mpe)
add_subdirectory(${MU_ROOT}/src/engraving engraving)
set(MUSICXML_NO_INTERNAL ON)
add_subdirectory(${MU_ROOT}/src/importexport/musicxml musicxml)
include(${MU_ROOT}/src/framework/draw/cmake/SetupFreeType.cmake)
include(${MU_ROOT}/src/framework/draw/cmake/SetupHarfBuzz.cmake)
include(${MU_ROOT}/src/framework/audio/thirdparty/fluidsynth/fluidsynth.cmake)
add_library(native_fluidsynth STATIC ${FLUIDSYNTH_SRC}
    ${MU_ROOT}/src/framework/audio/thirdparty/fluidsynth/vorbis_decode.cpp
    ${MU_ROOT}/src/framework/audio/engine/internal/codecs/vorbisdecoder.cpp)
target_include_directories(native_fluidsynth PUBLIC ${FLUIDSYNTH_INC} ${MU_ROOT}/src/framework/audio)
target_compile_definitions(native_fluidsynth PUBLIC ${FLUIDSYNTH_DEF} FLUIDSYNTH_NOT_A_DLL PRIVATE EXTERN_VORBIS_SUPPORT)
add_library(native_audio STATIC ${NATIVE_HOST}/native/offline_audio.cpp)
target_link_libraries(native_audio PUBLIC native_fluidsynth)
target_include_directories(native_audio PRIVATE ${MU_ROOT}/src/framework)
add_executable(offline_audio_probe ${NATIVE_HOST}/native/offline_audio_probe.cpp)
target_link_libraries(offline_audio_probe PRIVATE native_audio)
add_library(noqt_support STATIC
    ${NATIVE_HOST}/native/noqt_filesystem.cpp
    ${NATIVE_HOST}/native/noqt_settings.cpp
    ${MU_ROOT}/src/importexport/musicxml/internal/musicxmlconfiguration.cpp
    ${MU_ROOT}/src/framework/draw/internal/fontprovider.cpp
    ${MU_ROOT}/src/framework/draw/internal/fontsdatabase.cpp
    ${MU_ROOT}/src/framework/draw/internal/fontsengine.cpp
    ${MU_ROOT}/src/framework/draw/internal/fontfaceft.cpp
    ${MU_ROOT}/src/framework/draw/internal/fontfacedu.cpp
    ${MU_ROOT}/src/importexport/midi/internal/midishared/midifile.cpp
    ${MU_ROOT}/src/importexport/midi/internal/midiexport/exportmidi.cpp
)
target_compile_definitions(noqt_support PUBLIC MUSE_MODULE_DRAW_USE_QTTEXTDRAW)
target_include_directories(noqt_support PUBLIC ${MU_ROOT} ${MU_ROOT}/src ${MU_ROOT}/src/framework ${MU_ROOT}/src/framework/global
    ${MU_ROOT}/src/engraving ${MU_ROOT}/src/framework/draw ${FREETYPE_INCLUDE_DIRS} ${HARFBUZZ_INCLUDE_DIRS} ${PROJECT_BINARY_DIR})
target_link_libraries(noqt_support PUBLIC engraving muse_draw muse_global muse_mpe iex_musicxml ${FREETYPE_LIBRARIES} ${HARFBUZZ_LIBRARIES})
add_executable(noqt_probe ${NATIVE_HOST}/native/noqt_probe.cpp)
target_link_libraries(noqt_probe PRIVATE noqt_support)
add_executable(noqt_support_test ${NATIVE_HOST}/native/noqt_support_test.cpp)
target_link_libraries(noqt_support_test PRIVATE noqt_support)
find_package(nlohmann_json CONFIG REQUIRED)
find_path(CAIRO_INCLUDE_DIR cairo.h PATH_SUFFIXES cairo REQUIRED)
find_library(CAIRO_LIBRARY NAMES cairo REQUIRED)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NATIVE_HOST}/native/tools.json")
file(READ "${NATIVE_HOST}/native/tools.json" TOOL_SCHEMA)
string(JSON TOOL_SCHEMA_TYPE TYPE "${TOOL_SCHEMA}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/tool_schema.inc" "R\"schema(${TOOL_SCHEMA})schema\"")
add_executable(noqt_mcp
    ${NATIVE_HOST}/native/main.cpp
    ${NATIVE_HOST}/native/noqt_engine.cpp
    ${NATIVE_HOST}/native/noqt_runtime.cpp
    ${NATIVE_HOST}/native/composition.cpp
    ${NATIVE_HOST}/native/mcp_transport.cpp
    ${NATIVE_HOST}/native/atomic_file.cpp
    ${NATIVE_HOST}/native/score_metadata.cpp
    ${NATIVE_HOST}/native/score_positions.cpp
    ${NATIVE_HOST}/native/score_render.cpp
    ${NATIVE_HOST}/native/cairo_painter.cpp)
target_include_directories(noqt_mcp PRIVATE ${CAIRO_INCLUDE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(noqt_mcp PRIVATE noqt_support native_audio nlohmann_json::nlohmann_json ${CAIRO_LIBRARY})
set_target_properties(noqt_mcp PROPERTIES OUTPUT_NAME musescore-mcp)
if(MSVC)
    target_sources(noqt_probe PRIVATE ${NATIVE_HOST}/native/utf8.manifest)
    target_sources(offline_audio_probe PRIVATE ${NATIVE_HOST}/native/utf8.manifest)
    target_sources(noqt_mcp PRIVATE ${NATIVE_HOST}/native/utf8.manifest)
endif()
