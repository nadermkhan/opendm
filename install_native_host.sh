#!/usr/bin/env bash
set -euo pipefail

HOST_NAME="com.opendm.host"
DEFAULT_EXT_ID="ekehfpkieofbbgakjbmefknbbabjpoog"
FIREFOX_EXT_ID="extension@opendm.org"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOST_PATH="${1:-}"
EXTRA_EXT_ID="${2:-}"

if [ -z "$HOST_PATH" ]; then
    for candidate in \
        "$SCRIPT_DIR/opendm-native-host" \
        "$SCRIPT_DIR/build/opendm-native-host" \
        "$HOME/.local/bin/opendm-native-host" \
        "/usr/local/bin/opendm-native-host" \
        "/usr/bin/opendm-native-host"; do
        if [ -x "$candidate" ]; then
            HOST_PATH="$(cd "$(dirname "$candidate")" && pwd)/$(basename "$candidate")"
            break
        fi
    done
fi

if [ -z "$HOST_PATH" ] || [ ! -x "$HOST_PATH" ]; then
    echo "Error: Could not find executable 'opendm-native-host'." >&2
    echo "Build the project first or pass the binary path: $0 /path/to/opendm-native-host [extra_chromium_id]" >&2
    exit 1
fi

XDG_CFG="${XDG_CONFIG_HOME:-$HOME/.config}"

write_chrome_manifest() {
    local dest="$1"
    mkdir -p "$(dirname "$dest")"
    if [ -n "$EXTRA_EXT_ID" ] && [ "$EXTRA_EXT_ID" != "$DEFAULT_EXT_ID" ]; then
        cat > "$dest" <<EOF
{
  "name": "${HOST_NAME}",
  "description": "OpenDM Native Messaging Host",
  "path": "${HOST_PATH}",
  "type": "stdio",
  "allowed_origins": [
    "chrome-extension://${DEFAULT_EXT_ID}/",
    "chrome-extension://${EXTRA_EXT_ID}/"
  ]
}
EOF
    else
        cat > "$dest" <<EOF
{
  "name": "${HOST_NAME}",
  "description": "OpenDM Native Messaging Host",
  "path": "${HOST_PATH}",
  "type": "stdio",
  "allowed_origins": [
    "chrome-extension://${DEFAULT_EXT_ID}/"
  ]
}
EOF
    fi
}

write_firefox_manifest() {
    local dest="$1"
    mkdir -p "$(dirname "$dest")"
    cat > "$dest" <<EOF
{
  "name": "${HOST_NAME}",
  "description": "OpenDM Native Messaging Host",
  "path": "${HOST_PATH}",
  "type": "stdio",
  "allowed_extensions": [
    "${FIREFOX_EXT_ID}"
  ]
}
EOF
}

for dir in \
    "$XDG_CFG/google-chrome/NativeMessagingHosts" \
    "$XDG_CFG/chromium/NativeMessagingHosts" \
    "$XDG_CFG/BraveSoftware/Brave-Browser/NativeMessagingHosts" \
    "$XDG_CFG/microsoft-edge/NativeMessagingHosts" \
    "$XDG_CFG/vivaldi/NativeMessagingHosts" \
    "$XDG_CFG/opera/NativeMessagingHosts"; do
    write_chrome_manifest "$dir/${HOST_NAME}.json"
done

for dir in \
    "$HOME/.mozilla/native-messaging-hosts" \
    "$HOME/.librewolf/native-messaging-hosts"; do
    write_firefox_manifest "$dir/${HOST_NAME}.json"
done

echo "============================================================"
echo " OpenDM Native Messaging Host registered!"
echo " Host Binary: $HOST_PATH"
echo " Chromium Extension ID: $DEFAULT_EXT_ID"
echo " Firefox Extension ID:  $FIREFOX_EXT_ID"
echo "============================================================"
echo "Chromium/Edge/Brave (Unpacked):"
echo "  Run: node extension/scripts/build.mjs"
echo "  Load Unpacked -> dist/extension/chromium"
echo "Firefox:"
echo "  Install dist/opendm_extension.xpi (or dist/extension/firefox/manifest.json in about:debugging)"
echo ""
echo "Note: Flatpak or Snap-sandboxed browsers isolate host NativeMessagingHosts paths."
echo "      Use native .deb/.rpm packages or grant native-messaging portal permission if sandboxed."
