#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SVG_SRC="${SCRIPT_DIR}/data/icons/opendm.svg"
ICONS_BASE="${SCRIPT_DIR}/data/icons/hicolor"

if [ ! -f "${SVG_SRC}" ]; then
    echo "Error: Source SVG not found at ${SVG_SRC}"
    exit 1
fi

SIZES=(16 24 32 48 64 128 256 512)

echo "Generating hicolor PNG icons from ${SVG_SRC}..."

for s in "${SIZES[@]}"; do
    TARGET_DIR="${ICONS_BASE}/${s}x${s}/apps"
    mkdir -p "${TARGET_DIR}"
    rsvg-convert -w "${s}" -h "${s}" "${SVG_SRC}" -o "${TARGET_DIR}/opendm.png"
    echo "  Rendered ${s}x${s} icon: ${TARGET_DIR}/opendm.png"
done

# Ensure scalable directory has the SVG as well
mkdir -p "${ICONS_BASE}/scalable/apps"
cp -f "${SVG_SRC}" "${ICONS_BASE}/scalable/apps/opendm.svg"
echo "  Copied scalable SVG: ${ICONS_BASE}/scalable/apps/opendm.svg"

echo "Icon generation completed successfully."
