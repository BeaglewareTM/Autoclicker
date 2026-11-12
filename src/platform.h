// NYC - platform layer (implemented in platform_win.cpp / platform_mac.mm)
#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>

namespace plat {

using Clock = std::chrono::steady_clock;
using KeyFn = std::function<void(const std::string &id, bool down)>;  // keyboard + side mouse buttons
using PickFn = std::function<void(int x, int y)>;                     // real left clicks (for "Add position")
using PermFn = std::function<void(bool ok)>;                          // macOS permission state

const char *name();  // "win" or "mac"

// Window (called on the UI thread)
void early_init();
void *create_window(int logical_w, int logical_h);  // Windows: our own hidden frameless window. Mac: nullptr.
void attach(void *window, void *widget, std::function<void()> on_close);
void set_browser(void *controller);  // Windows: WebView2 controller (to set its background colour)
void win_resize(int logical_w, int logical_h);
void win_center();
void win_show();
void win_minimize();
void win_on_top(bool on);
void win_theme(bool light, bool translucent, unsigned border_rgb, unsigned bg_rgb);
void win_drag_begin();
void win_drag_to(int dx, int dy);  // offset in DIPs from where the drag started

// Global input
void start_hooks(KeyFn on_key, PickFn on_pick, PermFn on_perm);
void stop_hooks();

// Clicking (any thread). btn: 0 left, 1 right, 2 middle. Never clicks our own window.
void click(int btn, bool dbl);
void click_at(int x, int y, int btn);
// Press and release one key (or a side/middle mouse button) by id. Skipped while NYC itself is focused.
void press_key(const std::string &id);

// Timing for the click thread
void engine_thread_begin();  // high-res timer + priority
void engine_thread_end();
bool wait_until(Clock::time_point t, const std::atomic<bool> &running);  // false if stopped

// Keys
std::string key_name(const std::string &id);
std::string default_hotkey();
std::string default_killkey();
std::string default_keyhot();
std::string default_qh_key();  // Booga Quick heal menu key (C)
bool is_escape(const std::string &id);
bool is_clear_key(const std::string &id);

std::filesystem::path config_dir();

// Custom app icon. choose_icon_file opens a file picker and returns the chosen path ("" if cancelled).
// apply_icon(path) sets the taskbar/Dock icon from an image file (false if it can't be read);
// apply_icon("") restores the built-in icon.
std::filesystem::path choose_icon_file();
// Settings backup: Save-as dialog / Open dialog for .json files ("" if cancelled)
std::filesystem::path choose_export_file();
std::filesystem::path choose_import_file();
bool apply_icon(const std::filesystem::path &image);
void show_error(const std::string &msg);
[[noreturn]] void quit_now();

// "Type while clicking": while on, keys typed into the game are held back and replayed into
// Roblox chat between clicks (press / to open chat, type them, then clicking continues).
// keep = key ids that must never be held back (the hotkeys).
// Booga Quick heal: screenshot the screen and look for a Bloodfruit in an open inventory.
// Returns true with its screen position. Only ever called when the user presses the hotkey.
bool find_food(int &x, int &y);

void chat_relay(bool on, const std::vector<std::string> &keep);
// Called by the click engine before each click. Types anything waiting; false if nothing was waiting.
bool chat_flush();

// Auto update. update_fetch() checks GitHub releases of `repo` ("owner/name") for a version
// newer than `current`; if there is one it downloads the asset NYC.exe next to the running exe
// and returns true. Runs on a background thread, never throws. Returns false on Mac / no internet.
bool update_fetch(const char *repo, const char *current);
// Swap the downloaded exe in and launch it. Call only after settings are saved.
[[noreturn]] void update_install_and_restart();
// Remove leftovers from a previous update (old exe). Called once at startup.
void update_cleanup();

}  // namespace plat
