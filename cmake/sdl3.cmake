# SDL3 pinned to an exact release tarball and hash. For offline builds point
# FETCHCONTENT_SOURCE_DIR_SDL3 at an extracted copy of the same release.
include(FetchContent)

set(VNPSP_SDL3_VERSION 3.4.18)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-${VNPSP_SDL3_VERSION}/SDL3-${VNPSP_SDL3_VERSION}.tar.gz
    URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(SDL3)
