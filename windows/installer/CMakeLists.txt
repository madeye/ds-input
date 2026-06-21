# CMakeLists.txt — the DS Input universal installer (a single elevated exe that
# embeds both the x64 and ARM64 builds and installs the host-matching one).
#
# Driven by build-installer.ps1, which first runs ../build.ps1 to populate
# ../dist/<arch>/ (the payloads embedded by DSInputInstaller.rc). Build the
# installer itself as x64 so one exe runs on x64 (native) and ARM64 (emulated).

cmake_minimum_required(VERSION 3.20)
project(dsime_installer LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The DS Input installer builds only on Windows with MSVC.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(DSInputInstaller WIN32
    DSInputInstaller.cpp
    DSInputInstaller.rc
)
set_target_properties(DSInputInstaller PROPERTIES OUTPUT_NAME "DSInputInstaller")

# We embed our own manifest (elevation + common controls) via the .rc, so tell
# the linker not to also emit its default one (avoids a duplicate-manifest error).
target_link_options(DSInputInstaller PRIVATE "/MANIFEST:NO")
