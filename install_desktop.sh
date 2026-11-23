#!/usr/bin/env bash
set -euo pipefail

# OpenDM Desktop Installer for Linux
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Detect binary location (tarball root or ./build directory)
if [ -x "$SCRIPT_DIR/opendm" ]; then
    BIN_SRC_DIR="$SCRIPT_DIR"
elif [ -x "$SCRIPT_DIR/build/opendm" ]; then
    BIN_SRC_DIR="$SCRIPT_DIR/build"
else
    echo "Error: opendm binary not found in $SCRIPT_DIR or $SCRIPT_DIR/build" >&2
    exit 1
fi

echo "Installing OpenDM from $BIN_SRC_DIR..."

mkdir -p "$HOME/.local/bin"
mkdir -p "$HOME/.local/share/applications"
mkdir -p "$HOME/.local/share/icons/hicolor"
mkdir -p "$HOME/.local/share/opendm/ui_assets"

cp "$BIN_SRC_DIR/opendm" "$HOME/.local/bin/opendm"
chmod +x "$HOME/.local/bin/opendm"

if [ -x "$BIN_SRC_DIR/opendm-native-host" ]; then
    cp "$BIN_SRC_DIR/opendm-native-host" "$HOME/.local/bin/opendm-native-host"
    chmod +x "$HOME/.local/bin/opendm-native-host"
fi

if [ -d "$SCRIPT_DIR/data/icons/hicolor" ]; then
    cp -r "$SCRIPT_DIR/data/icons/hicolor/"* "$HOME/.local/share/icons/hicolor/"
fi
if [ -d "$SCRIPT_DIR/data/ui_assets" ]; then
    cp -r "$SCRIPT_DIR/data/ui_assets/"* "$HOME/.local/share/opendm/ui_assets/"
fi
if [ -f "$SCRIPT_DIR/src/ui/style.css" ]; then
    cp "$SCRIPT_DIR/src/ui/style.css" "$HOME/.local/share/opendm/style.css"
elif [ -f "$SCRIPT_DIR/data/style.css" ]; then
    cp "$SCRIPT_DIR/data/style.css" "$HOME/.local/share/opendm/style.css"
fi

for ext_src in "$SCRIPT_DIR/dist/extension" "$SCRIPT_DIR/extension"; do
    if [ -d "$ext_src/chromium" ] || [ -d "$ext_src/firefox" ]; then
        mkdir -p "$HOME/.local/share/opendm/extension"
        cp -r "$ext_src/"* "$HOME/.local/share/opendm/extension/"
        break
    fi
done
for pkg in "$SCRIPT_DIR/opendm_extension.xpi" "$SCRIPT_DIR/dist/opendm_extension.xpi" \
           "$SCRIPT_DIR/opendm-extension-chromium.zip" "$SCRIPT_DIR/dist/opendm-extension-chromium.zip"; do
    if [ -f "$pkg" ]; then
        cp "$pkg" "$HOME/.local/share/opendm/"
    fi
done

if [ -f "$SCRIPT_DIR/data/opendm.desktop" ]; then
    sed "s|^Exec=.*|Exec=$HOME/.local/bin/opendm %u|" \
        "$SCRIPT_DIR/data/opendm.desktop" > "$HOME/.local/share/applications/opendm.desktop"
fi

if [ -x "$HOME/.local/bin/opendm-native-host" ]; then
    "$SCRIPT_DIR/install_native_host.sh" "$HOME/.local/bin/opendm-native-host" || true
fi

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$HOME/.local/share/applications" || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "$HOME/.local/share/icons/hicolor" || true
fi

echo "Done! You can now launch OpenDM from your applications menu or run 'opendm'."
