# Toolchain file for the PSP. Wraps the one shipped with the pspdev toolchain,
# which also provides create_pbp_file(). Normally used through scripts/psp-build.sh.
if(NOT DEFINED ENV{PSPDEV})
    message(FATAL_ERROR "PSPDEV is not set; build inside the pspdev container (scripts/psp-build.sh)")
endif()

set(_pspdev_toolchain "$ENV{PSPDEV}/psp/share/pspdev.cmake")
if(NOT EXISTS "${_pspdev_toolchain}")
    message(FATAL_ERROR "pspdev toolchain file not found at ${_pspdev_toolchain}")
endif()
include("${_pspdev_toolchain}")
