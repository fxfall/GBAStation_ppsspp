cmake_minimum_required(VERSION 3.16)

project(libromx VERSION 0.2.0 LANGUAGES C)

include(GNUInstallDirs)
include(CTest)
include(CMakePackageConfigHelpers)

option(ROMX_BUILD_TESTS "Build libromx tests" ${BUILD_TESTING})
option(ROMX_BUILD_EXAMPLES "Build libromx C and C++ examples" OFF)
set(ROMX_CONFORMANCE_FIXTURE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../romx/tests/fixtures"
    CACHE PATH "Path to ROMX conformance and reference fixtures")

add_library(romx
    src/crc32.c
    src/error.c
    src/extract.c
    src/footer.c
    src/io_file.c
    src/json.c
    src/metadata.c
    src/mutable.c
    src/mutable_bundle.c
    src/mutable_stats.c
    src/mutable_write.c
    src/payload_view.c
    src/payload_file.c
    src/ridx.c
    src/png.c
    src/probe.c
    src/region.c
    src/reader.c
    src/registry.c
    src/sha256.c
    src/validate.c
    src/vfs_file.c
    src/writer.c
)
add_library(ROMX::romx ALIAS romx)

target_compile_features(romx PUBLIC c_std_99)
set_target_properties(romx PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

target_include_directories(romx
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(romx
        PRIVATE ROMX_BUILDING_LIBRARY
        PUBLIC ROMX_SHARED
    )
endif()

if(MSVC)
    target_compile_options(romx PRIVATE /W4)
else()
    target_compile_options(romx PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

install(TARGETS romx
    EXPORT romx-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES
    README.md
    docs/INTEGRATION.md
    docs/WRITER.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
install(FILES
    examples/write_romx.c
    examples/write_romx.cpp
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples
)

configure_package_config_file(
    cmake/romx-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/romx-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/romx
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/romx-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(EXPORT romx-targets
    FILE romx-targets.cmake
    NAMESPACE ROMX::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/romx
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/romx-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/romx-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/romx
)

if(ROMX_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(ROMX_BUILD_EXAMPLES)
    enable_language(CXX)
    add_subdirectory(examples)
endif()
