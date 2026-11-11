#include "../src/platform.h"
#include <thread>
std::atomic<long> CLICKS{0}; std::atomic<long> AT{0}; std::atomic<long> KEYS{0};
namespace plat {
const char *name() { return "test"; }
void early_init() {}
void *create_window(int, int) { return nullptr; }
void attach(void *, void *, std::function<void()>) {}
void set_browser(void *) {}
void win_resize(int, int) {} void win_center() {} void win_show() {} void win_minimize() {}
void win_on_top(bool) {} void win_theme(bool, bool, unsigned, unsigned) {} void win_drag_begin() {} void win_drag_to(int, int) {}
void start_hooks(KeyFn, PickFn, PermFn) {} void stop_hooks() {}
void click(int, bool dbl) { CLICKS += dbl ? 2 : 1; }
void click_at(int, int, int) { CLICKS++; AT++; }

void press_key(const std::string &) { KEYS++; }
void engine_thread_begin() {} void engine_thread_end() {}
bool wait_until(Clock::time_point t, const std::atomic<bool> &r) {
  using namespace std::chrono;
  while (Clock::now() < t) { if (!r) return false; auto rem = t - Clock::now();
    if (rem > microseconds(500)) std::this_thread::sleep_for(std::min<Clock::duration>(rem - microseconds(300), milliseconds(10))); else std::this_thread::yield(); }
  return true; }
std::string key_name(const std::string &id) { return id.empty() ? "Not set" : id == "vk:117" ? "F6" : id == "vk:45" ? "Insert" : id; }
std::string default_hotkey() { return "vk:117"; } std::string default_killkey() { return "vk:45"; }
std::string default_keyhot() { return "vk:118"; }
bool is_escape(const std::string &id) { return id == "vk:27"; }
bool is_clear_key(const std::string &id) { return id == "vk:8"; }
std::filesystem::path config_dir() { return "/tmp/beagle_test_cfg"; }
void show_error(const std::string &) {}
std::filesystem::path choose_icon_file() { return {}; }
bool update_fetch(const char *, const char *) { return false; }
void update_install_and_restart() { std::abort(); }
void update_cleanup() {}
std::filesystem::path TEST_EXPORT, TEST_IMPORT;
std::filesystem::path choose_export_file() { return TEST_EXPORT; }
std::filesystem::path choose_import_file() { return TEST_IMPORT; }
bool apply_icon(const std::filesystem::path &) { return true; }
void quit_now() { std::exit(0); }
}
