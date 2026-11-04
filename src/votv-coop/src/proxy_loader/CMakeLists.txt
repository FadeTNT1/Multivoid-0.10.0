# multivoid-loader -- the dwmapi.dll proxy that starts main.dll without UE4SS.
#
# Independent on purpose: no vcpkg, no GNS, no third_party, no sources of the mod. It builds either from the
# mod's CMakeLists (add_subdirectory, beside main.dll) or on its own for a scratch compile:
#   cmake -S src/votv-coop/src/proxy_loader -B build/proxy-loader -G "Visual Studio 17 2022" -A x64
#   cmake --build build/proxy-loader --config Release
# Output: dwmapi.dll (OUTPUT_NAME) in <build dir>/<config>/, beside main.dll when built from the mod's tree.
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(multivoid_loader LANGUAGES CXX ASM_MASM)
endif()

add_library(multivoid-loader SHARED
    loader.cpp
    forward.cpp
    dwmapi_stubs.asm
    dwmapi_proxy.def
)
set_target_properties(multivoid-loader PROPERTIES
    OUTPUT_NAME dwmapi
    PDB_NAME multivoid-loader
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    # static CRT: nothing to find in the game's folder
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_include_directories(multivoid-loader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    # /GS-: no stack cookie, so the stubs and the APC path touch nothing of the CRT before the exe's own startup.
    target_compile_options(multivoid-loader PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /GS->)
endif()
# DllCanUnloadNow / DllGetClassObject are forwarded like every other export; the COM-entry-point warning is noise.
target_link_options(multivoid-loader PRIVATE /IGNORE:4104)
