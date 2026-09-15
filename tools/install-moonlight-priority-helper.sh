#!/bin/sh
set -eu

SOURCE=${1:-./moonlight-priority-helper}
DEST_DIR=/opt/moonlight-priority
DEST=${DEST_DIR}/moonlight-priority-helper

if [ ! -f "$SOURCE" ]; then
    echo "Helper binary not found: $SOURCE" >&2
    exit 1
fi

if ! command -v setcap >/dev/null 2>&1; then
    echo "setcap is not available on this SteamOS installation." >&2
    exit 1
fi

sudo install -d -o root -g root -m 0755 "$DEST_DIR"
sudo install -o root -g root -m 0755 "$SOURCE" "$DEST"
sudo setcap cap_sys_nice=ep "$DEST"

CAPS=$(getcap "$DEST" 2>/dev/null || true)
case "$CAPS" in
    *cap_sys_nice*) ;;
    *)
        echo "Failed to verify cap_sys_nice on $DEST" >&2
        exit 1
        ;;
esac

echo "Installed Moonlight priority helper:"
echo "  $DEST"
echo "  $CAPS"
echo "No SteamOS read-only-root change was required."
