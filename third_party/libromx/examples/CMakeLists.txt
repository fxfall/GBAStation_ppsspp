add_executable(romx_write_c write_romx.c)
target_link_libraries(romx_write_c PRIVATE ROMX::romx)
target_compile_features(romx_write_c PRIVATE c_std_99)

add_executable(romx_write_cpp write_romx.cpp)
target_link_libraries(romx_write_cpp PRIVATE ROMX::romx)
target_compile_features(romx_write_cpp PRIVATE cxx_std_11)

if(MSVC)
    target_compile_options(romx_write_cpp PRIVATE /EHsc)
endif()
