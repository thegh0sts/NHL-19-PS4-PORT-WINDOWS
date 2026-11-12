#!/usr/bin/env bash
# Wrap the Linux tar.gz (scripts/package-linux.sh) into an AppImage, without rebuilding or
# changing its contents. BB_PRODUCT: the package's name (default NHL19Native).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
product=${BB_PRODUCT:-NHL19Native}
tool=${APPIMAGETOOL:-appimagetool}
tool=$(command -v -- "$tool") || {
    echo 'package-appimage: appimagetool is required. Install it on PATH, or set APPIMAGETOOL=/absolute/path/to/appimagetool (an executable x86_64 AppImage also works).' >&2
    exit 1
}
# Resolve relative overrides before appimagetool changes directories internally.
tool=$(realpath -- "$tool")
archive=dist/$product-linux-x86_64.tar.gz
[[ -f $archive ]] || { echo "package-appimage: missing $archive: run bash scripts/package-linux.sh first." >&2; exit 1; }

mkdir -p out dist
work=$(mktemp -d "$PWD/out/appimage.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
appdir=$work/$product.AppDir
mkdir -p "$appdir/usr/lib"
tar -xzf "$archive" -C "$appdir/usr/lib"
payload=$appdir/usr/lib/$product
for entry in nhl19.sh run.sh scripts/prepare.py scripts/merge_update.py bin/sacredfire; do
    [[ -f $payload/$entry ]] || { echo "package-appimage: Linux archive is missing $entry" >&2; exit 1; }
done
install -m755 packaging/linux/AppRun "$appdir/AppRun"
sed -i 's/\r$//' "$appdir/AppRun"
# The icon is a PNG: appimagetool wants .DirIcon to be one (an SVG is refused by newer versions).
install -m644 "packaging/linux/$product.desktop" "packaging/linux/$product.png" "$appdir/"
cp "$appdir/$product.png" "$appdir/.DirIcon"

# Python stays on the host, as with the tar.gz. Do not add bin/lib to LD_LIBRARY_PATH here: those
# libraries belong to the game. APPIMAGE_EXTRACT_AND_RUN lets appimagetool run without FUSE.
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$tool" "$appdir" "$work/$product-x86_64.AppImage"
install -m755 "$work/$product-x86_64.AppImage" "dist/$product-x86_64.AppImage"
(cd dist && sha256sum "$product-x86_64.AppImage" > "$product-x86_64.AppImage.sha256")
echo "package-appimage: dist/$product-x86_64.AppImage"
