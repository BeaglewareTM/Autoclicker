#!/bin/bash
# Builds "NYC.app" for macOS 10.15+ (Intel and Apple Silicon).
# Needs Apple's free command line tools: run  xcode-select --install  once.
set -euo pipefail
cd "$(dirname "$0")"
APP="build/NYC.app"
mkdir -p build
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

echo "Compiling..."
clang++ -std=c++17 -O2 -DNDEBUG -arch arm64 -arch x86_64 -mmacosx-version-min=10.15 \
  -Ivendor src/main.cpp src/platform_mac.mm \
  -framework Cocoa -framework WebKit -framework ApplicationServices -framework QuartzCore \
  -o "$APP/Contents/MacOS/NYC"

cp icon.icns "$APP/Contents/Resources/icon.icns"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>NYC</string>
  <key>CFBundleDisplayName</key><string>NYC</string>
  <key>CFBundleIdentifier</key><string>com.nyc.autoclicker</string>
  <key>CFBundleExecutable</key><string>NYC</string>
  <key>CFBundleIconFile</key><string>icon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleVersion</key><string>1.0.0</string>
  <key>CFBundleShortVersionString</key><string>1.0.0</string>
  <key>LSMinimumSystemVersion</key><string>10.15</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST

# Ad-hoc signature so macOS remembers the Accessibility permission between launches
codesign --force --deep --sign - "$APP"
echo
echo "Done: $APP"
echo "First launch: right-click the app > Open. Then allow it in"
echo "System Settings > Privacy & Security > Accessibility (and Input Monitoring)."
open build
