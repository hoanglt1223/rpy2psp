add_library(plat STATIC common/plat_common.c)
target_include_directories(plat
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)

if(PSP)
    target_sources(plat PRIVATE psp/psp_core.c psp/psp_audio.c psp/psp_files.c)
    target_link_libraries(plat PUBLIC pspgu pspge pspdisplay pspctrl pspaudio psppower m)
else()
    find_package(OpenGL REQUIRED)
    target_sources(plat PRIVATE sdl/sdl_core.c sdl/sdl_audio.c sdl/sdl_files.c)
    target_link_libraries(plat PUBLIC SDL3::SDL3 OpenGL::GL)
    if(NOT WIN32)
        target_link_libraries(plat PUBLIC m)
    endif()
endif()
