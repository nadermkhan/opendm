#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -f "${HOME}/.local/bin/opendm-native-host" ]; then
    HOST_BIN="${HOME}/.local/bin/opendm-native-host"
elif [ -f "${SCRIPT_DIR}/build/opendm-native-host" ]; then
    HOST_BIN="${SCRIPT_DIR}/build/opendm-native-host"
else
    echo "Error: Native messaging host binary not found in ~/.local/bin or build directory."
    echo "Please build and run ./install_desktop.sh first."
    exit 1
fi

MANIFEST_CONTENT_CHROME=$(cat <<EOF
{
  "name": "com.tonec.idm",
  "description": "OpenDM Native Messaging Host for Linux (IDM Compatible)",
  "path": "${HOST_BIN}",
  "type": "stdio",
  "allowed_origins": [
    "chrome-extension://ngpampappnmepgilojfohadhhmbhlaek/",
    "chrome-extension://bgdpgdfcmienfknnkgcfbgjjffmnokch/"
  ]
}
EOF
)

MANIFEST_CONTENT_FIREFOX=$(cat <<EOF
{
  "name": "com.tonec.idm",
  "description": "OpenDM Native Messaging Host for Linux (IDM Compatible)",
  "path": "${HOST_BIN}",
  "type": "stdio",
  "allowed_extensions": [
    "mozilla_cc@tonec.com",
    "idm_integration@tonec.com"
  ]
}
EOF
)

# Chrome / Chromium / Brave / Edge paths
CHROME_TARGETS=(
    "${HOME}/.config/google-chrome/NativeMessagingHosts"
    "${HOME}/.config/chromium/NativeMessagingHosts"
    "${HOME}/.config/BraveSoftware/Brave-Browser/NativeMessagingHosts"
    "${HOME}/.config/microsoft-edge/NativeMessagingHosts"
)

for dir in "${CHROME_TARGETS[@]}"; do
    mkdir -p "${dir}"
    echo "${MANIFEST_CONTENT_CHROME}" > "${dir}/com.tonec.idm.json"
    echo "Installed Chrome manifest to ${dir}/com.tonec.idm.json"
done

# Firefox paths
FIREFOX_TARGETS=(
    "${HOME}/.mozilla/native-messaging-hosts"
    "${HOME}/.config/mozilla/native-messaging-hosts"
)

for dir in "${FIREFOX_TARGETS[@]}"; do
    mkdir -p "${dir}"
    echo "${MANIFEST_CONTENT_FIREFOX}" > "${dir}/com.tonec.idm.json"
    echo "Installed Firefox manifest to ${dir}/com.tonec.idm.json"
done

echo "Native messaging manifests successfully registered."
