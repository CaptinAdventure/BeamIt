#!/bin/sh
# Builds the macOS client in Release and zips BeamIt.app into dist/BeamIt-macOS.zip.
# Run on a Mac with Xcode from the repository root:  sh scripts/build-macos-release.sh
# Without arguments the app is ad-hoc signed (runs on this Mac). Pass a team ID to sign for other Macs:
#   sh scripts/build-macos-release.sh ABCDE12345
set -eu

repository=$(cd "$(dirname "$0")/.." && pwd)
build="$repository/clients/osx/BeamIt/build"
dist="$repository/dist"

if [ "$#" -ge 1 ]; then
    signing="DEVELOPMENT_TEAM=$1"
else
    signing="CODE_SIGN_IDENTITY=- CODE_SIGN_STYLE=Manual DEVELOPMENT_TEAM="
fi

# shellcheck disable=SC2086 # $signing holds several build settings.
xcodebuild -project "$repository/clients/osx/BeamIt/BeamIt.xcodeproj" -target BeamIt \
    -configuration Release SYMROOT="$build" $signing build

mkdir -p "$dist"
rm -f "$dist/BeamIt-macOS.zip"
ditto -c -k --keepParent "$build/Release/BeamIt.app" "$dist/BeamIt-macOS.zip"
echo "Packaged $build/Release/BeamIt.app and $dist/BeamIt-macOS.zip"
echo "On another Mac, an ad-hoc signed copy needs: xattr -dr com.apple.quarantine /Applications/BeamIt.app"
