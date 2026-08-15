cmake_minimum_required(VERSION 3.16)
project(romx_package_consumer LANGUAGES C)

find_package(romx CONFIG REQUIRED)

add_executable(romx_package_consumer main.c)
target_link_libraries(romx_package_consumer PRIVATE ROMX::romx)
