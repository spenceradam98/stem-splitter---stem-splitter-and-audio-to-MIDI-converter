#!/usr/bin/env bash
# Builds stem-scribe-<version>-mac.dmg containing stem-scribe-Installer.pkg, which installs the
# VST3, the Audio Unit, the standalone app and the HTDemucs separation model (shared by all three).
# Usage: packaging/mac/make_installer.sh <build dir> <version> <out dir> <model file>
set -euo pipefail

BUILD="${1:?build dir, e.g. build}"
VERSION="${2:?version}"
OUT="${3:?output dir}"
MODEL="${4:?path to ggml-model-htdemucs-4s-f16.bin}"
NAME="stem-scribe"
ID="com.yourbrand.stemscribe"
HERE="$(cd "$(dirname "$0")" && pwd)"
ART="$BUILD/StemScribe_artefacts/Release"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT" "$WORK/pkgs"

component() { # $1 = path to copy, $2 = install location, $3 = id suffix
    local root="$WORK/root-$3"
    mkdir -p "$root"
    ditto "$1" "$root/$(basename "$1")"
    pkgbuild --analyze --root "$root" "$WORK/$3.plist"
    # keep bundles where we put them (no relocation to a copy found elsewhere on disk)
    local i=0; while /usr/libexec/PlistBuddy -c "Set :$i:BundleIsRelocatable false" "$WORK/$3.plist" 2>/dev/null; do i=$((i+1)); done
    pkgbuild --root "$root" --component-plist "$WORK/$3.plist" --identifier "$ID.$3" --version "$VERSION" \
             --install-location "$2" "$WORK/pkgs/$NAME-$3.pkg"
}

component "$ART/VST3/$NAME.vst3"       "/Library/Audio/Plug-Ins/VST3"       vst3
component "$ART/AU/$NAME.component"    "/Library/Audio/Plug-Ins/Components" au
component "$ART/Standalone/$NAME.app"  "/Applications"                      app

# the model is a plain file, not a bundle
MROOT="$WORK/root-model"
mkdir -p "$MROOT"
cp "$MODEL" "$MROOT/"
pkgbuild --root "$MROOT" --identifier "$ID.model" --version "$VERSION" \
         --install-location "/Library/Application Support/YourBrand/stem-scribe" "$WORK/pkgs/$NAME-model.pkg"

sed "s/@VERSION@/$VERSION/g" "$HERE/distribution.xml" > "$WORK/distribution.xml"
productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK/pkgs" \
             --resources "$HERE/resources" "$WORK/$NAME-Installer.pkg"

DMGROOT="$WORK/dmg"
mkdir -p "$DMGROOT"
cp "$WORK/$NAME-Installer.pkg" "$DMGROOT/"
cp "$HERE/resources/HOW TO INSTALL.txt" "$DMGROOT/"
hdiutil create -volname "$NAME $VERSION" -srcfolder "$DMGROOT" -ov -format UDZO \
        "$OUT/$NAME-$VERSION-mac.dmg"
echo "Created $OUT/$NAME-$VERSION-mac.dmg"
