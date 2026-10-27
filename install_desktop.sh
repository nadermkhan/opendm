#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_DIR="${HOME}/.local/bin"
APP_DIR="${HOME}/.local/share/applications"
ICON_DIR="${HOME}/.local/share/icons/hicolor/scalable/apps"

mkdir -p "${BIN_DIR}" "${APP_DIR}" "${ICON_DIR}"

# 1. Install executables
if [ -f "${SCRIPT_DIR}/build/opendm" ]; then
    cp -f "${SCRIPT_DIR}/build/opendm" "${BIN_DIR}/opendm"
    chmod +x "${BIN_DIR}/opendm"
    echo "Installed opendm binary to ${BIN_DIR}/opendm"
fi

if [ -f "${SCRIPT_DIR}/build/opendm-native-host" ]; then
    cp -f "${SCRIPT_DIR}/build/opendm-native-host" "${BIN_DIR}/opendm-native-host"
    chmod +x "${BIN_DIR}/opendm-native-host"
    echo "Installed opendm-native-host binary to ${BIN_DIR}/opendm-native-host"
fi

# 2. Install application icon
cp -f "${SCRIPT_DIR}/data/icons/opendm.svg" "${ICON_DIR}/opendm.svg"
echo "Installed icon to ${ICON_DIR}/opendm.svg"

# 3. Install desktop entry
cp -f "${SCRIPT_DIR}/data/opendm.desktop" "${APP_DIR}/opendm.desktop"
echo "Installed desktop entry to ${APP_DIR}/opendm.desktop"

# 4. Install authentic IDM UI assets, extension & stylesheet
SHARE_DIR="${HOME}/.local/share/opendm"
mkdir -p "${SHARE_DIR}/ui_assets"
cp -rf "${SCRIPT_DIR}/data/ui_assets/"* "${SHARE_DIR}/ui_assets/"
cp -f "${SCRIPT_DIR}/src/ui/style.css" "${SHARE_DIR}/style.css"
if [ -f "${SCRIPT_DIR}/opendm_extension.xpi" ]; then
    cp -f "${SCRIPT_DIR}/opendm_extension.xpi" "${SHARE_DIR}/opendm_extension.xpi"
fi
cp -rf "${SCRIPT_DIR}/extension_extracted" "${SHARE_DIR}/extension"
echo "Installed authentic IDM assets, extension, and stylesheet to ${SHARE_DIR}"

# 4. Update desktop database & icon cache if available
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "${APP_DIR}" 2>/dev/null || true
fi

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "${HOME}/.local/share/icons/hicolor" 2>/dev/null || true
fi

# 5. Register native messaging hosts
"${SCRIPT_DIR}/install_native_host.sh"

echo ""
echo "=== OpenDM Linux Desktop Installation Completed ==="
echo "You can now launch OpenDM from your system Application Menu or run 'opendm'."
