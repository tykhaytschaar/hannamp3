#!/usr/bin/env bash
# Docker-based ESP-IDF build for hannamp3.
# A buildet egy konténerben futtatja; a flashelés a hoston történik (esptool).
#
# Használat:
#   tools/build.sh                # idf.py build
#   tools/build.sh menuconfig     # idf.py menuconfig (interaktív, -it)
#   tools/build.sh fullclean
#   tools/build.sh reconfigure
#   tools/build.sh size-components
#   tools/build.sh shell          # interaktív bash a konténerben
#
# Env override:
#   IDF_IMAGE=espressif/idf:v5.3.1 tools/build.sh
#   IDF_TARGET=esp32s3 tools/build.sh
set -euo pipefail

IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v5.3.1}"
IDF_TARGET="${IDF_TARGET:-esp32s3}"
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cmd=("$@")
if [[ ${#cmd[@]} -eq 0 ]]; then
    cmd=(build)
fi

# Az első parancs az "shell": interaktív bash a konténerben.
if [[ "${cmd[0]}" == "shell" ]]; then
    exec docker run --rm -it \
        -v "$PROJECT_ROOT":/project \
        -w /project \
        "$IDF_IMAGE" \
        bash
fi

# Interaktivitás csak menuconfighoz kell.
DOCKER_TTY="-t"
if [[ "${cmd[0]}" == "menuconfig" ]]; then
    DOCKER_TTY="-it"
fi

# Első futás: set-target (a sdkconfig hiányzik vagy más targetre van állítva).
INIT_CMD=""
if [[ ! -f "$PROJECT_ROOT/sdkconfig" ]]; then
    INIT_CMD="idf.py set-target ${IDF_TARGET} && "
fi

# shellcheck disable=SC2068
exec docker run --rm ${DOCKER_TTY} \
    -v "$PROJECT_ROOT":/project \
    -w /project \
    "$IDF_IMAGE" \
    bash -lc "${INIT_CMD}idf.py ${cmd[*]}"
