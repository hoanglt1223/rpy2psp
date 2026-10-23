# Third-party code

- **Platform layer** (`third_party/psp-platform/`, MIT, same author): vendored
  unmodified from AnySWF; source commit in its `VENDORED.md`. `vnpsp/src/png_write.c`
  and `cmake/sdl3.cmake`, `cmake/psp.cmake`, `docker/psp-build.Dockerfile` and
  `scripts/psp-build.sh` are adapted from the same repository.
- **minimp3** (`third_party/minimp3/`, CC0 1.0): MP3 decoding in the engine;
  commit in its `VENDORED.md`.
- **SDL3** (zlib license): fetched at configure time for the PC build (pinned
  release and hash in `cmake/sdl3.cmake`), not stored in the repository.
- **pspsdk / pspdev toolchain** (BSD and others): used from the pinned
  `pspdev/pspdev` Docker image to build the PSP target.

Python dependencies are listed with their versions in `requirements.txt`.
