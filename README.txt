BEAGLE AUTOCLICKER (C++)

WINDOWS
  Just run "NYC.exe". Nothing to install.
  Settings are saved in %APPDATA%\BeagleAutoclicker\settings.json
  Tip: if a game runs as administrator, right-click the exe > Run as administrator,
  otherwise Windows blocks clicks into that game.

MAC (macOS 10.15 or newer)
  1. Open Terminal once and run:  xcode-select --install
  2. In Terminal, drag in build_mac.sh and press Enter. It makes build/NYC.app
  3. First launch: right-click the app > Open.
  4. Allow it in System Settings > Privacy & Security > Accessibility AND Input Monitoring
     (needed to click and to hear your hotkeys). The app says when access is missing.
  Settings are saved in ~/Library/Application Support/BeagleAutoclicker/settings.json
  Default hotkeys on Mac: F6 to start/stop (you may need fn+F6), Insert/Help as killswitch.

CHANGING THE CODE
  ui/index.html  - the interface. After editing run:  python tools/gen_ui.py
  src/main.cpp   - settings, click engine, hotkeys
  src/platform_win.cpp / src/platform_mac.mm - Windows / Mac specific parts
  build_windows.bat - rebuilds the exe (needs Visual Studio Build Tools or MinGW)
