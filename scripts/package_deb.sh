#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${SCRIPT_DIR}"

PACKAGE_NAME="opendm"
VERSION="1.0.0-1"
ARCH="amd64"
DIST_DIR="${SCRIPT_DIR}/dist"
STAGE_DIR="${SCRIPT_DIR}/build/deb_staging/${PACKAGE_NAME}_${VERSION}_${ARCH}"

echo "=== Packaging OpenDM ${VERSION} (${ARCH}) for Debian / Ubuntu ==="

# 1. Generate multi-resolution icons
"${SCRIPT_DIR}/scripts/generate_icons.sh"

# 2. Re-compile build tree
mkdir -p "${SCRIPT_DIR}/build"
cmake -B "${SCRIPT_DIR}/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "${SCRIPT_DIR}/build" -j"$(nproc)"

# 3. Clean and prepare staging directory
rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}"

# 4. Install files to staging directory
DESTDIR="${STAGE_DIR}" cmake --install "${SCRIPT_DIR}/build" --prefix /usr

# 5. Install system-wide Chrome/Chromium native messaging manifests
mkdir -p "${STAGE_DIR}/etc/opt/chrome/native-messaging-hosts"
mkdir -p "${STAGE_DIR}/etc/chromium/native-messaging-hosts"
cp -f "${SCRIPT_DIR}/data/native-messaging/com.tonec.idm.chrome.json" "${STAGE_DIR}/etc/opt/chrome/native-messaging-hosts/com.tonec.idm.json"
cp -f "${SCRIPT_DIR}/data/native-messaging/com.tonec.idm.chrome.json" "${STAGE_DIR}/etc/chromium/native-messaging-hosts/com.tonec.idm.json"

# 6. Compute installed size in KB
INSTALLED_SIZE=$(du -sk "${STAGE_DIR}" | cut -f1)

# 7. Create DEBIAN/control
mkdir -p "${STAGE_DIR}/DEBIAN"
cat << CONTROL_EOF > "${STAGE_DIR}/DEBIAN/control"
Package: ${PACKAGE_NAME}
Version: ${VERSION}
Section: net
Priority: optional
Architecture: ${ARCH}
Depends: libc6 (>= 2.34), libcurl4t64 | libcurl4 (>= 7.80.0), libgtk-4-1t64 | libgtk-4-1 (>= 4.6.0), libstdc++6 (>= 11), zlib1g (>= 1:1.2.11)
Recommends: ffmpeg
Installed-Size: ${INSTALLED_SIZE}
Maintainer: OpenDM Development Team <contact@opendm.org>
Homepage: https://github.com/nadermkhan/opendm
Description: High-speed dynamic multipart download manager (IDM Architecture)
 OpenDM is a native Linux download accelerator engineered with authentic
 Internet Download Manager (IDM) segmentation algorithms, multi-browser
 integration via native messaging, and YouTube 4K stream extraction with
 automatic media muxing.
CONTROL_EOF

# 8. Create DEBIAN/postinst
cat << 'POSTINST_EOF' > "${STAGE_DIR}/DEBIAN/postinst"
#!/bin/sh
set -e

if [ "$1" = "configure" ]; then
    if which update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications || true
    fi
    if which gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
    fi
    if which update-mime-database >/dev/null 2>&1; then
        update-mime-database /usr/share/mime || true
    fi
fi
exit 0
POSTINST_EOF
chmod 755 "${STAGE_DIR}/DEBIAN/postinst"

# 9. Create DEBIAN/postrm
cat << 'POSTRM_EOF' > "${STAGE_DIR}/DEBIAN/postrm"
#!/bin/sh
set -e

if [ "$1" = "remove" ] || [ "$1" = "purge" ]; then
    if which update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications || true
    fi
    if which gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
    fi
    if which update-mime-database >/dev/null 2>&1; then
        update-mime-database /usr/share/mime || true
    fi
fi
exit 0
POSTRM_EOF
chmod 755 "${STAGE_DIR}/DEBIAN/postrm"

# 10. Build the .deb package
mkdir -p "${DIST_DIR}"
OUTPUT_DEB="${DIST_DIR}/${PACKAGE_NAME}_${VERSION}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "${STAGE_DIR}" "${OUTPUT_DEB}"

echo ""
echo "=== Package Build Successful ==="
echo "Artifact: ${OUTPUT_DEB}"
echo ""
dpkg-deb -I "${OUTPUT_DEB}"
echo ""
echo "Contents summary:"
dpkg-deb -c "${OUTPUT_DEB}" | head -n 35
echo "... (Total files: $(dpkg-deb -c "${OUTPUT_DEB}" | wc -l))"
