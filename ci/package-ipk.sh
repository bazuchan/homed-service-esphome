#!/bin/sh
# Packages a built homed-esphome binary as an .ipk (opkg format), for either a
# native OpenWrt (opkg) target or a Keenetic/Entware target.
# Usage: ci/package-ipk.sh <target-id> <version> <binary>
# Output: <repo-root>/homed-esphome_<version>_<arch>.ipk
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
FLAVOR="$(echo "$TARGET" | jq -r '.flavor')"

case "$FLAVOR" in
    openwrt|entware) ;;
    *)
        echo "target $TARGET_ID has no ipk flavor (expected openwrt or entware, got '$FLAVOR')" >&2
        exit 1
        ;;
esac

PACKAGE="homed-esphome_${VERSION}_${ARCHITECTURE}"
STAGING="$(mktemp -d)"
DATA="$STAGING/data"
CONTROL="$STAGING/control"

mkdir -p "$DATA" "$CONTROL"

# Anything already sitting under deploy/data (config, but also static assets
# shipped for other services -- e.g. deploy/data/usr/share/homed-web/js/services
# is homed-web plugin JS that belongs in *this* package) rides along as-is.
cp -a "$REPO_DIR/deploy/data/." "$DATA/"

if [ "$FLAVOR" = "openwrt" ]; then
    mkdir -p "$DATA/etc/init.d" "$DATA/usr/bin" "$DATA/opt/homed-esphome"
    cp "$BINARY" "$DATA/usr/bin/homed-esphome"
    cp "$REPO_DIR/deploy/procd/homed-esphome" "$DATA/etc/init.d/homed-esphome"
    chmod +x "$DATA/etc/init.d/homed-esphome"

    cp "$REPO_DIR/deploy/opkg/conffiles" "$CONTROL/conffiles"
    cp "$REPO_DIR/deploy/opkg/postinst" "$CONTROL/postinst"
    cp "$REPO_DIR/deploy/opkg/prerm" "$CONTROL/prerm"
    chmod +x "$CONTROL/postinst" "$CONTROL/prerm"
    sed "s/^Version:.*/Version: ${VERSION}/; s/^Architecture:.*/Architecture: ${ARCHITECTURE}/" \
        "$REPO_DIR/deploy/opkg/control" > "$CONTROL/control"
else
    # Entware/Keenetic: everything lives under /opt, matching homed-common's
    # .deploy_entware job -- /etc/homed/<conf> -> /opt/etc/homed (with the conf's
    # own /opt/<name> -> /opt/var/lib/<name> and /var/log -> /opt/var/log paths
    # rewritten), and /usr/share/* -> /opt/share/* wholesale (not just this
    # package's own name) so shared web assets land at the same final path
    # regardless of which service's package installs them -- matching wherever
    # homed-web's own Entware package puts its assets.
    if [ -f "$DATA/etc/homed/homed-esphome.conf" ]; then
        mkdir -p "$DATA/opt/etc/homed"
        sed -e "s+/opt/homed-esphome+/opt/var/lib/homed-esphome+g" -e "s+/var/log+/opt/var/log+g" \
            "$DATA/etc/homed/homed-esphome.conf" > "$DATA/opt/etc/homed/homed-esphome.conf"
    fi
    if [ -d "$DATA/usr/share" ] && [ -n "$(ls -A "$DATA/usr/share")" ]; then
        mkdir -p "$DATA/opt/share"
        mv "$DATA/usr/share/"* "$DATA/opt/share/"
    fi
    rm -rf "$DATA/etc" "$DATA/usr"

    mkdir -p "$DATA/opt/etc/init.d" "$DATA/opt/bin" "$DATA/opt/var/lib/homed-esphome"
    cp "$BINARY" "$DATA/opt/bin/homed-esphome"
    cp "$REPO_DIR/deploy/entware/S88homed-esphome" "$DATA/opt/etc/init.d/S88homed-esphome"
    chmod +x "$DATA/opt/etc/init.d/S88homed-esphome"

    cp "$REPO_DIR/deploy/entware/conffiles" "$CONTROL/conffiles"
    sed "s/^Version:.*/Version: ${VERSION}/; s/^Architecture:.*/Architecture: ${ARCHITECTURE}/" \
        "$REPO_DIR/deploy/entware/control" > "$CONTROL/control"
fi

(cd "$DATA" && fakeroot tar -czf "$STAGING/data.tar.gz" .)
(cd "$CONTROL" && fakeroot tar -czf "$STAGING/control.tar.gz" .)
echo "2.0" > "$STAGING/debian-binary"

(cd "$STAGING" && fakeroot tar -czf "$REPO_DIR/${PACKAGE}.ipk" control.tar.gz data.tar.gz debian-binary)
rm -rf "$STAGING"

echo "Packaged: ${PACKAGE}.ipk"
