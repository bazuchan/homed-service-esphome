#!/bin/sh
# Builds homed-esphome for a single target from ci/targets.json.
# Usage: ci/build.sh <target-id> [version]
# Output: <repo-root>/homed-esphome-<target-id>
set -e

TARGET_ID="$1"
if [ -z "$TARGET_ID" ]; then
    echo "usage: $0 <target-id> [version]" >&2
    exit 1
fi

# Stamped into the binary as SERVICE_VERSION (see homed-esphome.pro); falls
# back to device.h's default if not given, same as package-*.sh's own default.
export SERVICE_VERSION="${2:-0.1.0}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TARGETS_JSON="$SCRIPT_DIR/targets.json"

TARGET="$(jq -c --arg id "$TARGET_ID" '.targets[] | select(.id == $id)' "$TARGETS_JSON")"
if [ -z "$TARGET" ]; then
    echo "unknown target: $TARGET_ID (see ci/targets.json)" >&2
    exit 1
fi

if [ "$(echo "$TARGET" | jq -r '.enabled')" != "true" ]; then
    echo "target $TARGET_ID is disabled: $(echo "$TARGET" | jq -r '._note // "no reason given"')" >&2
    exit 1
fi

TOOLCHAIN="$(echo "$TARGET" | jq -r '.toolchain')"
QT_BUILD="$(echo "$TARGET" | jq -r '.qt_build')"

if [ -z "$TOOLCHAIN" ] || [ "$TOOLCHAIN" = "null" ]; then
    echo "target $TARGET_ID has no toolchain configured" >&2
    exit 1
fi

CACHE_DIR="${HOMED_TOOLCHAIN_CACHE:-$HOME/.cache/homed-toolchains}"
mkdir -p "$CACHE_DIR"

# fetch <kind: gcc|qt> <name> -- downloads+extracts once, prints the extracted dir
fetch()
{
    kind="$1"
    name="$2"
    dest="$CACHE_DIR/$kind/$name"

    if [ ! -d "$dest" ]; then
        echo "Downloading $kind/$name.tar.xz..." >&2
        mkdir -p "$dest.tmp"
        curl -fsSL "https://sandbox.u236.org/toolchain/$kind/$name.tar.xz" | tar -xJ -C "$dest.tmp" --strip-components=1
        mv "$dest.tmp" "$dest"
    fi

    echo "$dest"
}

QT_DIR="$(fetch qt "$QT_BUILD")"

COMMON_DIR="$REPO_DIR/../homed-common"
if [ ! -d "$COMMON_DIR" ]; then
    echo "Cloning homed-service-common into $COMMON_DIR..." >&2
    git clone https://github.com/u236/homed-service-common "$COMMON_DIR"
fi

cd "$REPO_DIR"
rm -f .qmake.stash
rm -rf home Makefile.bak moc_* homed-esphome
find . -name '*.o' -not -path './noise-c/.git/*' -delete

case "$TOOLCHAIN" in
    docker:*)
        # linux-amd64's qt-amd64-linux-5.15.4-shared build links dynamically against
        # system ICU 67, which most current host distros no longer ship -- run the
        # build inside a matching Debian-bullseye-era image instead, same as
        # homed-common's own gitlab job for this target. Cross targets don't hit
        # this: their Qt builds are ICU-free (self-contained embedded builds).
        IMAGE="${TOOLCHAIN#docker:}"
        docker run --rm -u "$(id -u):$(id -g)" \
            -e SERVICE_VERSION \
            -v "$QT_DIR":/opt/qt-target:ro \
            -v "$REPO_DIR/..":/build \
            -w "/build/$(basename "$REPO_DIR")" \
            "$IMAGE" sh -c '/opt/qt-target/bin/qmake homed-esphome.pro && make -j"$(nproc)"'
        ;;
    native)
        # No cross-compiler needed -- use the runner's own gcc, only Qt is pinned.
        "$QT_DIR/bin/qmake" homed-esphome.pro
        make -j"$(nproc)"
        ;;
    *)
        GCC_DIR="$(fetch gcc "$TOOLCHAIN")"
        export PATH="$PATH:$GCC_DIR/bin"
        export STAGING_DIR="$GCC_DIR"
        "$QT_DIR/bin/qmake" homed-esphome.pro
        make -j"$(nproc)"
        ;;
esac

BINARY_OUT="$REPO_DIR/homed-esphome-$TARGET_ID"
cp homed-esphome "$BINARY_OUT"
echo "Built: $BINARY_OUT"
