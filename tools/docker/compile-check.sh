#!/usr/bin/env bash
# Builds the current checkout in a Linux x86_64 container, to check that it
# compiles (docs/NATIVE-ONLY.md, M0). It does not run the game.
#
#   tools/docker/compile-check.sh            # tools (once), recompile, build
#   tools/docker/compile-check.sh build      # only the game build (after a src/ change)
#   tools/docker/compile-check.sh shell      # a shell in the container
#
# Needs: Docker able to run linux/amd64 images, the submodules checked out
# (git submodule update --init), and disc/ with the extracted game
# (recompile reads the executable and the overlays). The build trees are kept
# in the Docker volume rg-build, mounted over build/, so the host checkout gets
# no Linux objects; `docker volume rm rg-build` starts from scratch.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IMAGE=rg-compile-check
VOLUME=rg-build
STEP="${1:-all}"

docker build --platform linux/amd64 -t "$IMAGE" "$ROOT/tools/docker" >&2

run() {
    docker run --rm --platform linux/amd64 \
        -v "$ROOT":/src -v "$VOLUME":/src/build \
        ${DOCKER_TTY:+-it} "$IMAGE" "$@"
}

case "$STEP" in
    shell) DOCKER_TTY=1 run bash ;;
    build) run python setup.py build ;;
    all)
        run bash -c '
            set -e
            [ -x build/tools/ps2xRecomp/ps2_recomp ] || python setup.py tools
            python setup.py recompile
            python setup.py build
        ' ;;
    *) echo "usage: $0 [all|build|shell]" >&2; exit 2 ;;
esac
