#!/usr/bin/env bash
# Builds the PSP target. Outside the container it builds the pinned image and
# re-runs itself inside it; inside (PSPDEV set) it runs CMake.
#
#   scripts/psp-build.sh [Release|Debug]     output: out/psp/vnpsp/EBOOT.PBP
#
# Extra CMake arguments go in PSP_CMAKE_ARGS, e.g. PSP_CMAKE_ARGS=-DBUILD_PRX=ON for psplink.
# `sh scripts/psp-build.sh` works too: re-run under bash when started by another shell (dash).
if [ -z "${BASH_VERSION:-}" ]; then exec bash "$0" "$@"; fi
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
config="${1:-Release}"
case "$config" in
    Release|Debug|RelWithDebInfo) ;;
    *) echo "psp-build: unknown build type '$config'" >&2; exit 2 ;;
esac

if [ -z "${PSPDEV:-}" ]; then
    command -v docker >/dev/null || { echo "psp-build: docker not found" >&2; exit 1; }
    image="vnpsp-psp-build:local"
    docker build -t "$image" -f "$root/docker/psp-build.Dockerfile" "$root/docker"
    # MSYS_NO_PATHCONV keeps Git Bash on Windows from rewriting the container paths.
    # On Linux, run as the calling user so out/psp is not owned by root.
    user_args=()
    if [ "$(uname -s)" = Linux ]; then user_args=(--user "$(id -u):$(id -g)" -e HOME=/tmp); fi
    MSYS_NO_PATHCONV=1 docker run --rm "${user_args[@]}" \
        -v "$root:/src" -w /src \
        -e PSP_CMAKE_ARGS="${PSP_CMAKE_ARGS:-}" \
        "$image" bash scripts/psp-build.sh "$config"
    exit $?
fi

build_dir="$root/out/psp"
# shellcheck disable=SC2086
cmake -S "$root" -B "$build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/psp.cmake" \
    -DCMAKE_BUILD_TYPE="$config" \
    ${PSP_CMAKE_ARGS:-}
cmake --build "$build_dir" --parallel

pbp="$build_dir/vnpsp/EBOOT.PBP"
[ -f "$pbp" ] || { echo "psp-build: $pbp was not produced" >&2; exit 1; }
echo "psp-build: $pbp"
