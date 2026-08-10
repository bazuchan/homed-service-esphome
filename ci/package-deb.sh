#!/bin/sh
# Packages a built homed-esphome binary as a .deb.
# Usage: ci/package-deb.sh <target-id> <version> <binary>
# Output: <repo-root>/homed-esphome_<version>_<arch>.deb
set -e

TARGET_ID="$1"
VERSION="$2"
BINARY="$3"

if [ -z "$TARGET_ID" ] || [ -z "$VERSION" ] || [ -z "$BINARY" ]; then
    echo "usage: $0 <target-id> <version> <binary>" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TARGETS_JSON="$SCRIPT_DIR/targets.json"

TARGET="$(jq -c --arg id "$TARGET_ID" '.targets[] | select(.id == $id)' "$TARGETS_JSON")"
if [ -z "$TARGET" ]; then
    echo "unknown target: $TARGET_ID" >&2
    exit 1
fi

ARCHITECTURE="$(echo "$TARGET" | jq -r '.architecture')"

# dpkg validates Architecture: against its own fixed name list -- "aarch64" (the
# uname/toolchain name used elsewhere for this target) isn't on it, "arm64" is.
case "$ARCHITECTURE" in
    aarch64) ARCHITECTURE=arm64 ;;
esac

PACKAGE="homed-esphome_${VERSION}_${ARCHITECTURE}"
STAGING="$(mktemp -d)"

# Anything already sitting under deploy/data (config, but also static assets
# shipped for other services -- e.g. deploy/data/usr/share/homed-web/js/services
# is homed-web plugin JS that belongs in *this* package) rides along as-is.
cp -a "$REPO_DIR/deploy/data/." "$STAGING/"

mkdir -p "$STAGING/DEBIAN"
mkdir -p "$STAGING/usr/bin"
mkdir -p "$STAGING/opt/homed-esphome"
mkdir -p "$STAGING/lib/systemd/system"

cp "$BINARY" "$STAGING/usr/bin/homed-esphome"
cp "$REPO_DIR/deploy/systemd/homed-esphome.service" "$STAGING/lib/systemd/system/"

sed "s/^Version:.*/Version: ${VERSION}/; s/^Architecture:.*/Architecture: ${ARCHITECTURE}/" \
    "$REPO_DIR/deploy/apt/control" > "$STAGING/DEBIAN/control"

cp "$REPO_DIR/deploy/apt/conffiles" "$STAGING/DEBIAN/conffiles"
cp "$REPO_DIR/deploy/apt/postinst" "$STAGING/DEBIAN/postinst"
cp "$REPO_DIR/deploy/apt/prerm" "$STAGING/DEBIAN/prerm"
chmod 755 "$STAGING/DEBIAN/postinst" "$STAGING/DEBIAN/prerm"

fakeroot dpkg-deb --root-owner-group --build "$STAGING" "$REPO_DIR/${PACKAGE}.deb"
rm -rf "$STAGING"

echo "Packaged: ${PACKAGE}.deb"
