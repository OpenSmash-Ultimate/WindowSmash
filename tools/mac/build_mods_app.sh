#!/bin/bash
# Builds build-mac/WindowSmash Mods.app (tools/mac/ModsApp.swift) with the Command Line Tools' Swift,
# and puts it in ~/Applications as "WindowSmash Mods" (Spotlight finds it). Run from anywhere.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
app="$root/build-mac/WindowSmash Mods.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
swiftc -parse-as-library -O -swift-version 5 -target arm64-apple-macos14.0 \
    "$root/tools/mac/ModsApp.swift" -o "$app/Contents/MacOS/WindowSmash Mods"
icon="$HOME/Applications/Play SSBU.app/Contents/Resources/windowsmash.icns"
[ -f "$icon" ] && cp "$icon" "$app/Contents/Resources/windowsmash.icns"
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>WindowSmash Mods</string>
  <key>CFBundleDisplayName</key><string>WindowSmash Mods</string>
  <key>CFBundleExecutable</key><string>WindowSmash Mods</string>
  <key>CFBundleIdentifier</key><string>io.github.sno2207.windowsmash.mods</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleIconFile</key><string>windowsmash</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>WindowSmashRoot</key><string>$root</string>
</dict></plist>
PLIST
codesign --force --sign - "$app" 2>/dev/null || true
mkdir -p "$HOME/Applications"
rm -rf "$HOME/Applications/WindowSmash Mods.app"
cp -R "$app" "$HOME/Applications/WindowSmash Mods.app"
echo "built: $app (and ~/Applications/WindowSmash Mods.app)"
