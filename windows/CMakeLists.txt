# CMakeLists.txt — DS Input Windows TSF text service + settings app.
#
# Produces two binaries:
#   dsime_tsf.dll        — the in-proc COM TSF text service (regsvr32 target).
#   DSInputSettings.exe  — standalone settings dialog launched from the IME menu.
#
# Both link the dsime core (built separately by Cargo as a cdylib). We link the
# MSVC import library produced alongside dsime.dll. build.ps1 drives the whole
# flow (cargo + cmake); you can also configure this directly with VS 2022.
#
# Requirements: Visual Studio 2022 (MSVC v143), Windows 10/11 SDK, x64.

cmake_minimum_required(VERSION 3.20)
project(dsime_windows LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR
        "The DS Input Windows frontend builds only on Windows with MSVC. "
        "On this host you can edit/review the sources but not compile them.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Unicode everywhere (TSF + Win32 wide APIs).
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# ---- locate the Rust core artifacts ---------------------------------------
# The Cargo build drops dsime.dll + its import lib here. Override on the command
# line with -DDSIME_CORE_DIR=... if you build the core elsewhere.
set(DSIME_CORE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../core/target/x86_64-pc-windows-msvc/release"
    CACHE PATH "Directory containing dsime.dll and its import library")

# MSVC names the cdylib import lib "dsime.dll.lib"; some toolchains emit
# "dsime.lib". Accept either.
find_library(DSIME_IMPORT_LIB
    NAMES dsime.dll dsime
    PATHS "${DSIME_CORE_DIR}"
    NO_DEFAULT_PATH)
if(NOT DSIME_IMPORT_LIB)
    message(WARNING
        "dsime import library not found in ${DSIME_CORE_DIR}. "
        "Run the Cargo build first (see build.ps1). Falling back to a bare "
        "'dsime' link name; configure -DDSIME_CORE_DIR to fix.")
    set(DSIME_IMPORT_LIB dsime)
    link_directories("${DSIME_CORE_DIR}")
endif()

set(DSIME_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../core/include")

# Windows libs every TSF service needs.
set(DSIME_SYS_LIBS
    ole32 oleaut32 uuid user32 gdi32 advapi32 shell32 imm32 comctl32)

# ---- the TSF DLL ----------------------------------------------------------
add_library(dsime_tsf SHARED
    dllmain.cpp
    Guids.cpp
    TextService.cpp
    KeyEventSink.cpp
    Composition.cpp
    EditSessions.cpp
    DisplayAttribute.cpp
    LangBarButton.cpp
    Registry.cpp
    dsime_tsf.rc
)

target_include_directories(dsime_tsf PRIVATE "${DSIME_INCLUDE_DIR}")
target_link_libraries(dsime_tsf PRIVATE "${DSIME_IMPORT_LIB}" ${DSIME_SYS_LIBS})

# Export the four COM entry points via the .def (clean, undecorated names).
set_target_properties(dsime_tsf PROPERTIES
    LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/dsime_tsf.def\""
    OUTPUT_NAME "dsime_tsf"
    PREFIX "")

# ---- the settings app -----------------------------------------------------
add_executable(DSInputSettings WIN32
    settings/DSInputSettings.cpp
    settings/DSInputSettings.rc
)
target_include_directories(DSInputSettings PRIVATE "${DSIME_INCLUDE_DIR}")
target_link_libraries(DSInputSettings PRIVATE "${DSIME_IMPORT_LIB}" ${DSIME_SYS_LIBS})
set_target_properties(DSInputSettings PROPERTIES OUTPUT_NAME "DSInputSettings")

# ---- post-build: stage dsime.dll next to our binaries ---------------------
# The text service and settings exe load dsime.dll at runtime; keep a copy in
# the output dir so local testing works without touching PATH.
add_custom_command(TARGET dsime_tsf POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DSIME_CORE_DIR}/dsime.dll"
        "$<TARGET_FILE_DIR:dsime_tsf>/dsime.dll"
    COMMENT "Staging dsime.dll next to dsime_tsf.dll")
