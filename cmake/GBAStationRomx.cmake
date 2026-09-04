# GBAStation ROMX integration for the PPSSPP core.
#
# This module intentionally owns all build-system changes for ROMX.  The only
# hook kept in PPSSPP's upstream CMakeLists.txt is the include() call.

set(ROMX_BUILD_TESTS OFF CACHE BOOL "Build libromx tests" FORCE)
set(ROMX_BUILD_EXAMPLES OFF CACHE BOOL "Build libromx examples" FORCE)
add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/libromx"
                 "${CMAKE_BINARY_DIR}/libromx" EXCLUDE_FROM_ALL)

target_sources(${CoreLibName} PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/FileLoaders/RomxFileLoader.cpp"
    "${CMAKE_SOURCE_DIR}/Core/FileLoaders/RomxFileLoader.h")
target_link_libraries(${CoreLibName} ROMX::romx)
