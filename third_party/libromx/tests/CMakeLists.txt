add_executable(romx_tests test_v2.c)
target_link_libraries(romx_tests PRIVATE ROMX::romx)
target_compile_features(romx_tests PRIVATE c_std_99)
set_target_properties(romx_tests PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)

if(MSVC)
    target_compile_options(romx_tests PRIVATE /W4)
    target_compile_definitions(romx_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(romx_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(EXISTS "${ROMX_CONFORMANCE_FIXTURE_DIR}/minimal-single.romx")
    add_test(NAME romx.0.2.0 COMMAND romx_tests
        "${ROMX_CONFORMANCE_FIXTURE_DIR}")
endif()

add_executable(romx_writer_mutable_tests test_writer_mutable.c)
target_link_libraries(romx_writer_mutable_tests PRIVATE ROMX::romx)
target_compile_features(romx_writer_mutable_tests PRIVATE c_std_99)
set_target_properties(romx_writer_mutable_tests PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)

if(MSVC)
    target_compile_options(romx_writer_mutable_tests PRIVATE /W4)
    target_compile_definitions(romx_writer_mutable_tests PRIVATE
        _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(romx_writer_mutable_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_test(NAME romx.writer-mutable COMMAND romx_writer_mutable_tests
    "${CMAKE_CURRENT_BINARY_DIR}")

add_executable(romx_probe_sample probe_sample.c)
target_link_libraries(romx_probe_sample PRIVATE ROMX::romx)
target_compile_features(romx_probe_sample PRIVATE c_std_99)
