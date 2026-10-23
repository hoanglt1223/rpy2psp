set(VNCORE_SOURCES
    src/assets.c src/atl.c src/audio.c src/base.c src/engine.c src/fs.c src/menu.c src/rollback.c src/save.c
    src/scene.c src/settings.c src/text.c src/transitions.c src/value.c src/vm.c src/vnb.c)

add_library(vncore STATIC ${VNCORE_SOURCES})
target_include_directories(vncore PUBLIC src ${PROJECT_SOURCE_DIR}/third_party/minimp3)
if(PSP)
    target_sources(vncore PRIVATE src/gfx_gu.c)
    target_link_libraries(vncore PUBLIC plat)
else()
    target_sources(vncore PRIVATE src/gfx_soft.c)
    if(NOT WIN32)
        target_link_libraries(vncore PUBLIC m)
    endif()
endif()
# minimp3 is third-party code: keep its warnings out of ours.
if(MSVC)
    set_source_files_properties(src/audio.c PROPERTIES COMPILE_OPTIONS "/wd4244;/wd4267;/wd4456;/wd4457;/wd4701")
else()
    set_source_files_properties(src/audio.c PROPERTIES COMPILE_OPTIONS "-Wno-sign-compare;-Wno-unused-parameter")
endif()

add_executable(vnpsp src/main.c)
target_link_libraries(vnpsp PRIVATE vncore plat)

if(PSP)
    create_pbp_file(
        TARGET vnpsp
        TITLE "vnpsp"
        VERSION 01.00
        ICON_PATH NULL
        BACKGROUND_PATH NULL
        PREVIEW_PATH NULL
    )
else()
    target_sources(vnpsp PRIVATE src/driver.c src/png_write.c)

    if(BUILD_TESTING)
        add_executable(vnpsp_tests tests/unit_tests.c)
        target_link_libraries(vnpsp_tests PRIVATE vncore)
        target_compile_definitions(vnpsp_tests PRIVATE VNPSP_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
        add_test(NAME unit COMMAND vnpsp_tests)
        # The rest builds and plays the self-written test game, so it needs Python with
        # requirements.txt installed (pass -DPython3_EXECUTABLE=... to pick that one).
        find_package(Python3 COMPONENTS Interpreter)
        if(Python3_FOUND)
            add_test(NAME engine-vs-reference
                     COMMAND ${Python3_EXECUTABLE} -m unittest tests.engine.test_engine tests.engine.test_target_games -v
                     WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
            set_tests_properties(engine-vs-reference PROPERTIES ENVIRONMENT "VNPSP_EXE=$<TARGET_FILE:vnpsp>")
            add_test(NAME smoke-game-build
                     COMMAND ${Python3_EXECUTABLE} -m tests.engine.make_test_game ${CMAKE_CURRENT_BINARY_DIR}/smoke-game
                     WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
            set_tests_properties(smoke-game-build PROPERTIES FIXTURES_SETUP smoke-game)
            # The real platform path: hidden SDL window and the silent dummy audio driver.
            add_test(NAME platform-smoke-headless
                     COMMAND vnpsp ${CMAKE_CURRENT_BINARY_DIR}/smoke-game --headless --frames 240)
            set_tests_properties(platform-smoke-headless PROPERTIES FIXTURES_REQUIRED smoke-game)
        endif()
    endif()
endif()
