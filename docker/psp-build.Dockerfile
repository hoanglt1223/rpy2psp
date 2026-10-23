# PSP build environment: pspdev toolchain (psp-gcc, pspsdk, CMake, make).
# Pinned by tag and digest; bump both together.
FROM pspdev/pspdev:v20261001@sha256:54895e6f5afb71b8f4f6915ee6d5023e1e087ca7afdf2dcb1d7a694a5233e45f

WORKDIR /src
CMD ["bash", "scripts/psp-build.sh"]
