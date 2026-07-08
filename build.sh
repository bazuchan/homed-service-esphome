#!/bin/sh
set -e

COMMON_DIR="../homed-common"
COMMON_REPO="https://github.com/u236/homed-service-common"

if [ ! -d "$COMMON_DIR" ]; then
    echo "Cloning homed-service-common into $COMMON_DIR..."
    git clone "$COMMON_REPO" "$COMMON_DIR"
fi

qmake homed-esphome.pro
make -j"$(nproc)"
