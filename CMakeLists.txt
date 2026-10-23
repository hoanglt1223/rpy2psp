cmake_minimum_required(VERSION 3.24)
project(vnpsp VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# pspsdk headers rely on GNU extensions.
if(NOT PSP)
    set(CMAKE_C_EXTENSIONS OFF)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Fetched before our warning flags so third-party code builds with its own settings.
if(NOT PSP)
    include(cmake/sdl3.cmake)
endif()

if(MSVC)
    add_compile_options(/W4 $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra)
endif()

if(NOT PSP)
    include(CTest)
endif()

add_subdirectory(third_party/psp-platform)
add_subdirectory(vnpsp)
