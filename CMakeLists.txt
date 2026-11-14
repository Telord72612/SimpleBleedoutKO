cmake_minimum_required(VERSION 3.21)

project(SimpleBleedoutKO LANGUAGES CXX VERSION 2.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# SimpleBleedoutKO - the C++ SKSE plugin.
# ONE DLL for Skyrim SE 1.5.97 / AE 1.6.x / 1.7.x / VR 1.4.15, built on MinLL/CommonLibVR 4.39.5 @550cc4fb (MIT; the
# CommonLib fork Injury Engine builds on). It is pinned as the git submodule extern/CommonLibVR:
#     git clone --recurse-submodules https://github.com/Telord72612/SimpleBleedoutKO.git
# (or `git submodule update --init --recursive` after a plain clone). Another CommonLib tree can be given with
# -DCOMMONLIB_PATH=<folder>; switching trees = wipe build/ first (an old CMakeCache.txt keeps the old COMMONLIB_PATH).
# ---------------------------------------------------------------------------
set(COMMONLIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibVR" CACHE PATH "Path to the CommonLib source tree")

set(ENABLE_SKYRIM_SE   ON  CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE   ON  CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR   ON  CACHE BOOL "" FORCE)
set(BUILD_TESTS        OFF CACHE BOOL "" FORCE)   # else CommonLib wants Catch2
set(SKSE_SUPPORT_XBYAK OFF CACHE BOOL "" FORCE)
# Always compile CommonLib from source: never fetch its published prebuilt bundle (cmake/Prebuilt.cmake).
set(COMMONLIB_PREBUILT OFF CACHE BOOL "" FORCE)

# No build-machine folder in the DLL: the source / header paths that __FILE__ and std::source_location put into the binary are
# trimmed to the part below this folder's parent (/d1trimfile, every target incl. CommonLib - set before add_subdirectory), and
# the PDB is named without its folder (/PDBALTPATH, below). String data only; the code is the same.
get_filename_component(SBKO_TRIM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
file(TO_NATIVE_PATH "${SBKO_TRIM_ROOT}" SBKO_TRIM_ROOT_NATIVE)
add_compile_options("/d1trimfile:${SBKO_TRIM_ROOT_NATIVE}")

add_subdirectory("${COMMONLIB_PATH}" CommonLibSSE EXCLUDE_FROM_ALL)

# Header-only JSON reader for Injury Engine's GetStateJson and SBKO's marks.json (vcpkg port nlohmann-json).
find_package(nlohmann_json CONFIG REQUIRED)

add_library(SimpleBleedoutKO SHARED
    src/main.cpp        # SKSE entry, log (.log0 kept), banner, messaging, co-save registration
    src/TestCmd.cpp     # the `sbko` console command (an unused vanilla donor slot)
    src/Settings.cpp    # SimpleBleedoutKO.ini (hand parser)
    src/Helper.cpp      # the SBKO_Helper.psc calls (KnockOut / WakeUp / SetRank / HealthPct)
    src/Knockout.cpp    # the knockout - sinks, check thread, state machine, adoption, co-save
    src/Api.cpp         # the SBKO_API Papyrus natives (ApiVersion / TakeOver / Holds / ChokeReleased)
    src/Marks.cpp       # the skin marks - Injury Engine mirror, the painter dispatch, the choke neck mark
    src/RefillGuard.cpp # the engine's full heal at combat end / NoBleedoutRecovery off skipped (VR 1.4.15 call + branch patch)
)

target_precompile_headers(SimpleBleedoutKO PRIVATE src/PCH.h)
target_link_libraries(SimpleBleedoutKO PRIVATE CommonLibSSE::CommonLibSSE nlohmann_json::nlohmann_json)
# Crash symbols: a PDB beside the Release DLL.
target_compile_options(SimpleBleedoutKO PRIVATE "$<$<CONFIG:Release>:/Zi>")
# /PDBALTPATH = the PDB's file name only (a literal: CMake's VS generator escapes "%_PDB%" into "%%%...%%%")
target_link_options(SimpleBleedoutKO PRIVATE "$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF;/PDBALTPATH:SimpleBleedoutKO.pdb>")
set_target_properties(SimpleBleedoutKO PROPERTIES
    OUTPUT_NAME "SimpleBleedoutKO"
    DEBUG_POSTFIX ""
)

# The build only STAGES its output (never copies into a game or mod folder): _stage/SKSE/Plugins = the DLL + PDB as they go
# into Data/, _symbols = a copy kept for reading crash logs.
set(SBKO_STAGE   "${CMAKE_CURRENT_SOURCE_DIR}/_stage/SKSE/Plugins")
set(SBKO_SYMBOLS "${CMAKE_CURRENT_SOURCE_DIR}/_symbols")
add_custom_command(TARGET SimpleBleedoutKO POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SBKO_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SBKO_SYMBOLS}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SimpleBleedoutKO>" "${SBKO_STAGE}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_PDB_FILE:SimpleBleedoutKO>" "${SBKO_STAGE}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SimpleBleedoutKO>" "${SBKO_SYMBOLS}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_PDB_FILE:SimpleBleedoutKO>" "${SBKO_SYMBOLS}/"
    COMMENT "Staging SimpleBleedoutKO.dll + .pdb to _stage/SKSE/Plugins and _symbols (NOT deployed)"
)
