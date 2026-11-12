// NYC - shared core: settings, click engine, hotkeys, UI bridge.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "json.hpp"
#ifdef BEAGLE_TEST
#include "../tests/mock_webview.h"
#else
#include "webview.h"
#endif
#include "platform.h"
#include "ui_html.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

static constexpr int WIN_W = 517;  // 470 css px at 110% zoom
static webview::webview *W = nullptr;

// ============================================================================
// Settings
// ============================================================================
struct Theme { unsigned border; bool light; unsigned bg; };
static const std::map<std::string, Theme> THEMES = {
    {"Dark", {0x1c1c1c, false, 0x080808}},     {"Light", {0xe2e2e5, true, 0xf4f4f5}},     {"Transparent", {0x2a2a2a, false, 0x0b0b0b}},
    {"Midnight", {0x181b2e, false, 0x05060d}}, {"Rose", {0x2c1a24, false, 0x120a0e}},     {"Sakura", {0xf6dbe5, true, 0xfff5f8}},
    {"Crimson", {0x2b1414, false, 0x0e0606}},  {"Sunset", {0x2e2016, false, 0x120c08}},   {"Gold", {0x2a2414, false, 0x0e0c06}},
    {"Coffee", {0x2d231c, false, 0x110d0a}},   {"Forest", {0x1a2d20, false, 0x08100b}},   {"Matcha", {0xdde8d4, true, 0xf3f7ef}},
    {"Mint", {0x163029, false, 0x061210}},     {"Ocean", {0x16283a, false, 0x071018}},    {"Ice", {0xd9e7f1, true, 0xf1f7fb}},
    {"Lavender", {0x252036, false, 0x0d0b14}}, {"Grape", {0x2a1636, false, 0x0d0612}},    {"Neon", {0x1a1a1a, false, 0x050505}},
    {"Mono", {0x222222, false, 0x0a0a0a}},
};

static int index_of(const std::string &v, std::initializer_list<const char *> opts) {
  int i = 0;
  for (auto o : opts) { if (v == o) return i; ++i; }
  return -1;
}

struct Settings {
  std::mutex m;
  bool armed = false;
  double cps = 20.0;
  std::string button = "Left", ctype = "Single", mode = "Toggle";
  int pos_delay = 30, burst = 10;
  std::vector<std::pair<int, int>> positions;
  std::string hotkey, killkey, theme = "Dark";
  bool on_top = false;
  bool chat_type = false;  // Type while clicking
  bool custom_icon = false;
  // Key Macro
  bool key_on = false;
  std::string key_target, key_hot, key_mode = "Toggle";
  double key_delay = 100.0;  // ms between presses, 0.01 minimum
  int key_burst = 10;
  // Booga: Heal and drop (press heal key, click heal spot, press drop key, click drop spot, repeat)
  bool hd_on = false;
  std::string hd_hot, hd_heal_key, hd_drop_key;
  int hd_heal_x = -1, hd_heal_y = -1, hd_drop_x = -1, hd_drop_y = -1;  // -1 = not set
  int hd_wait = 30;  // ms after each menu key before clicking

  fs::path file() { return plat::config_dir() / "settings.json"; }

  void sanitize() {
    if (index_of(button, {"Left", "Right", "Middle"}) < 0) button = "Left";
    if (index_of(ctype, {"Single", "Double", "Multi"}) < 0) ctype = "Single";
    if (index_of(mode, {"Toggle", "Hold", "Burst"}) < 0) mode = "Toggle";
    if (!std::isfinite(cps)) cps = 20.0;
    cps = std::clamp(std::round(cps * 10.0) / 10.0, 1.0, 1000.0);
    pos_delay = std::clamp(pos_delay, 0, 10000);
    burst = std::clamp(burst, 1, 10000);
    if (positions.size() > 500) positions.resize(500);
    if (!THEMES.count(theme)) theme = "Dark";
    if (killkey.empty()) killkey = plat::default_killkey();
    if (index_of(key_mode, {"Toggle", "Hold", "Burst"}) < 0) key_mode = "Toggle";
    if (!std::isfinite(key_delay)) key_delay = 100.0;
    key_delay = std::clamp(std::round(key_delay * 100.0) / 100.0, 0.01, 10000.0);
    key_burst = std::clamp(key_burst, 1, 10000);
    hd_wait = std::clamp(hd_wait, 5, 1000);
    if (hd_heal_x < 0 || hd_heal_y < 0) hd_heal_x = hd_heal_y = -1;
    if (hd_drop_x < 0 || hd_drop_y < 0) hd_drop_x = hd_drop_y = -1;
  }

  void load() { load_from(file()); }

  // Returns true if the file was a valid NYC settings file.
  bool load_from(const fs::path &path) {
    hotkey = plat::default_hotkey();
    killkey = plat::default_killkey();
    key_hot = plat::default_keyhot();
    hd_hot = plat::default_hdhot();
    hd_heal_key = plat::default_heal_key();
    hd_drop_key = plat::default_drop_key();
    bool ok = false;
    try {
      std::ifstream f(path);
      if (f) {
        json j = json::parse(f, nullptr, false);
        if (j.is_object()) {
          ok = true;
          auto get = [&](const char *k, auto &out) {
            if (j.contains(k)) { try { j.at(k).get_to(out); } catch (...) {} }
          };
          get("armed", armed); get("cps", cps); get("button", button); get("ctype", ctype);
          get("mode", mode); get("pos_delay", pos_delay); get("burst", burst);
          get("hotkey", hotkey); get("killkey", killkey); get("theme", theme); get("on_top", on_top); get("chat_type", chat_type); get("custom_icon", custom_icon);
          get("key_on", key_on); get("key_target", key_target); get("key_hot", key_hot); get("key_mode", key_mode);
          get("key_delay", key_delay); get("key_burst", key_burst);
          get("hd_on", hd_on); get("hd_hot", hd_hot); get("hd_heal_key", hd_heal_key); get("hd_drop_key", hd_drop_key);
          get("hd_wait", hd_wait);
          auto spot = [&](const char *k, int &x, int &y) {
            if (j.contains(k) && j[k].is_array() && j[k].size() == 2 && j[k][0].is_number() && j[k][1].is_number()) {
              x = j[k][0].get<int>(); y = j[k][1].get<int>();
            }
          };
          spot("hd_heal_spot", hd_heal_x, hd_heal_y);
          spot("hd_drop_spot", hd_drop_x, hd_drop_y);
          if (j.contains("positions") && j["positions"].is_array()) {
            for (auto &p : j["positions"])
              if (p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number())
                positions.emplace_back(p[0].get<int>(), p[1].get<int>());
          }
        }
      }
    } catch (...) { ok = false; }
    sanitize();
    return ok;
  }

  // Copy every setting from another Settings (used by Import). Caller holds this->m.
  void copy_from(const Settings &o) {
    armed = o.armed; cps = o.cps; button = o.button; ctype = o.ctype; mode = o.mode;
    pos_delay = o.pos_delay; burst = o.burst; positions = o.positions; hotkey = o.hotkey; killkey = o.killkey;
    theme = o.theme; on_top = o.on_top; chat_type = o.chat_type; custom_icon = o.custom_icon; key_on = o.key_on;
    key_target = o.key_target; key_hot = o.key_hot; key_mode = o.key_mode; key_delay = o.key_delay;
    key_burst = o.key_burst;
    hd_on = o.hd_on; hd_hot = o.hd_hot; hd_heal_key = o.hd_heal_key; hd_drop_key = o.hd_drop_key;
    hd_heal_x = o.hd_heal_x; hd_heal_y = o.hd_heal_y; hd_drop_x = o.hd_drop_x; hd_drop_y = o.hd_drop_y;
    hd_wait = o.hd_wait;
  }

  json to_json_locked() const {
    json pos = json::array();
    for (auto &p : positions) pos.push_back({p.first, p.second});
    return {{"armed", armed}, {"cps", cps}, {"button", button}, {"ctype", ctype}, {"mode", mode},
            {"pos_delay", pos_delay}, {"burst", burst}, {"positions", pos}, {"hotkey", hotkey},
            {"killkey", killkey}, {"theme", theme}, {"on_top", on_top}, {"chat_type", chat_type}, {"custom_icon", custom_icon},
            {"key_on", key_on}, {"key_target", key_target}, {"key_hot", key_hot}, {"key_mode", key_mode},
            {"key_delay", key_delay}, {"key_burst", key_burst},
            {"hd_on", hd_on}, {"hd_hot", hd_hot}, {"hd_heal_key", hd_heal_key}, {"hd_drop_key", hd_drop_key},
            {"hd_heal_spot", {hd_heal_x, hd_heal_y}}, {"hd_drop_spot", {hd_drop_x, hd_drop_y}}, {"hd_wait", hd_wait}};
  }

  void save() {
    std::string text;
    { std::lock_guard<std::mutex> l(m); text = to_json_locked().dump(1); }
    try {
      fs::create_directories(plat::config_dir());
      fs::path tmp = file();
      tmp += ".tmp";
      { std::ofstream o(tmp, std::ios::binary | std::ios::trunc); o << text; }
      fs::rename(tmp, file());  // atomic replace: never leaves a half-written file
    } catch (...) {}
  }
};
static Settings CFG;

// Snapshot the click thread reads every cycle (one short lock, no allocations unless Multi)
struct Snap { int btn; bool dbl, multi; double cps; int pos_delay; std::vector<std::pair<int, int>> pos; };
static Snap snapshot(bool with_positions) {
  std::lock_guard<std::mutex> l(CFG.m);
  Snap s{index_of(CFG.button, {"Left", "Right", "Middle"}), CFG.ctype == "Double", CFG.ctype == "Multi",
         CFG.cps, CFG.pos_delay, {}};
  if (s.multi && with_positions) s.pos = CFG.positions;
  return s;
}

// ============================================================================
// UI bridge helpers
// ============================================================================
static void js(std::string code) {
  if (W) W->dispatch([code = std::move(code)] { W->eval(code); });
}

// ============================================================================
// Click engine
// ============================================================================
class Engine {
 public:
  std::atomic<bool> running{false};
  // name: "click" or "key" (tells the page which status to update); body runs one action
  Engine(const char *name, std::function<bool(Engine &)> body, std::function<double()> interval_ms)
      : name_(name), body_(std::move(body)), interval_ms_(std::move(interval_ms)) {}

  void start(int burst) {
    std::lock_guard<std::mutex> l(m_);
    if (running) return;
    if (th_.joinable()) th_.join();  // previous run already finished or finishing (ms at most)
    running = true;
    js(std::string("onRunning('") + name_ + "',true)");
    th_ = std::thread([this, burst] { loop(burst); });
  }
  void stop() { running = false; }
  void toggle() { running ? stop() : start(0); }
  ~Engine() { running = false; if (th_.joinable()) th_.detach(); }

 private:
  const char *name_;
  std::function<bool(Engine &)> body_;
  std::function<double()> interval_ms_;
  std::mutex m_;
  std::thread th_;

  void loop(int burst) {
    plat::engine_thread_begin();
    using namespace std::chrono;
    auto next = plat::Clock::now();
    long done = 0;
    while (running) {
      if (!body_(*this)) break;
      if (burst > 0 && ++done >= burst) break;
      auto iv = duration_cast<plat::Clock::duration>(duration<double, std::milli>(std::max(0.01, interval_ms_())));
      next += iv;
      auto now = plat::Clock::now();
      if (next < now - iv) next = now;  // fell behind (PC hiccup): never burst to catch up
      if (!plat::wait_until(next, running)) break;
    }
    plat::engine_thread_end();
    if (std::string(name_) == "click") plat::chat_relay(false, {});
    running = false;
    js(std::string("onRunning('") + name_ + "',false)");
  }
};

// Auto clicker: one click (or one pass over the saved spots) per step
// Type while clicking: hold the user's typing back from the game, type it into chat between clicks.
static void chat_sync(bool on) {
  std::vector<std::string> keep;
  { std::lock_guard<std::mutex> l(CFG.m); keep = {CFG.hotkey, CFG.killkey, CFG.key_hot, CFG.hd_hot}; }
  plat::chat_relay(on, keep);
}
static bool click_step(Engine &e) {
  using namespace std::chrono;
  Snap s = snapshot(true);
  static thread_local bool relay = false;
  bool want;
  { std::lock_guard<std::mutex> l(CFG.m); want = CFG.chat_type; }
  if (want != relay) { relay = want; chat_sync(want); }
  if (want) plat::chat_flush();
  if (s.multi && !s.pos.empty()) {
    for (size_t i = 0; i < s.pos.size() && e.running; ++i) {
      plat::click_at(s.pos[i].first, s.pos[i].second, s.btn);
      if (i + 1 < s.pos.size() && s.pos_delay > 0 &&
          !plat::wait_until(plat::Clock::now() + milliseconds(s.pos_delay), e.running))
        return false;
    }
  } else {
    plat::click(s.btn, s.dbl);
  }
  return true;
}
static double click_interval() { std::lock_guard<std::mutex> l(CFG.m); return std::max(1.0, 1000.0 / std::max(1.0, CFG.cps)); }

// Key macro: one key press per step
static bool key_step(Engine &) {
  std::string k;
  { std::lock_guard<std::mutex> l(CFG.m); k = CFG.key_target; }
  if (k.empty()) return false;  // nothing to press
  plat::press_key(k);
  return true;
}
// Never faster than 10 ms per press (100/s). Games only read input once per frame (~60/s),
// and flooding key events makes the game process thousands of inputs and freeze.
static constexpr double MIN_INTERVAL_MS = 10.0;
static double key_interval() { std::lock_guard<std::mutex> l(CFG.m); return std::max(MIN_INTERVAL_MS, CFG.key_delay); }

// Booga Heal and drop: open heal menu, click heal spot, open shop, click chest button.
// Always alternates the two keys, so a menu key is never pressed twice in a row.
static bool hd_step(Engine &e) {
  using namespace std::chrono;
  std::string hk, dk;
  int hx, hy, dx, dy, w;
  {
    std::lock_guard<std::mutex> l(CFG.m);
    hk = CFG.hd_heal_key; dk = CFG.hd_drop_key; w = CFG.hd_wait;
    hx = CFG.hd_heal_x; hy = CFG.hd_heal_y; dx = CFG.hd_drop_x; dy = CFG.hd_drop_y;
  }
  if (hk.empty() || dk.empty() || hx < 0 || dx < 0) return false;  // not set up yet
  plat::press_key(hk);
  if (!plat::wait_until(plat::Clock::now() + milliseconds(w), e.running)) return false;
  plat::click_at(hx, hy, 0);
  if (!plat::wait_until(plat::Clock::now() + milliseconds(w), e.running)) return false;
  plat::press_key(dk);
  if (!plat::wait_until(plat::Clock::now() + milliseconds(w), e.running)) return false;
  plat::click_at(dx, dy, 0);
  return true;
}
static double hd_interval() { std::lock_guard<std::mutex> l(CFG.m); return (double)std::max(5, CFG.hd_wait); }

static Engine ENGINE("click", click_step, click_interval);
static Engine KENGINE("key", key_step, key_interval);
static Engine HDENGINE("hd", hd_step, hd_interval);

// ============================================================================
// Hotkeys (called on the hook thread - must stay fast)
// ============================================================================
static std::mutex g_state_m;
static std::string g_binding;  // "", "hotkey", "killkey", "key_hot" or "key_target"
static std::atomic<bool> g_picking{false};
static std::string g_pick_target;  // "" = Multi list, "hd_heal" / "hd_drop" = Booga spots (guarded by g_state_m)
static std::set<std::string> g_held;  // only touched on the hook thread

static std::string positions_json() {
  std::lock_guard<std::mutex> l(CFG.m);
  json pos = json::array();
  for (auto &p : CFG.positions) pos.push_back({p.first, p.second});
  return pos.dump();
}

static std::string *bind_slot(const std::string &key) {
  if (key == "hotkey") return &CFG.hotkey;
  if (key == "killkey") return &CFG.killkey;
  if (key == "key_hot") return &CFG.key_hot;
  if (key == "key_target") return &CFG.key_target;
  if (key == "hd_hot") return &CFG.hd_hot;
  if (key == "hd_heal_key") return &CFG.hd_heal_key;
  if (key == "hd_drop_key") return &CFG.hd_drop_key;
  return nullptr;
}

static void finish_bind(const std::string &id) {
  std::string key;
  { std::lock_guard<std::mutex> l(g_state_m); key.swap(g_binding); }
  if (key.empty()) return;
  std::string name;
  {
    std::lock_guard<std::mutex> l(CFG.m);
    std::string *slot = bind_slot(key);
    if (!slot) return;
    // NYC's own start/stop keys vs keys NYC presses for you
    const std::vector<std::pair<std::string, std::string *>> hots = {
        {"hotkey", &CFG.hotkey}, {"killkey", &CFG.killkey}, {"key_hot", &CFG.key_hot}, {"hd_hot", &CFG.hd_hot}};
    const std::vector<std::string *> presses = {&CFG.key_target, &CFG.hd_heal_key, &CFG.hd_drop_key};
    bool is_hotkey = key == "hotkey" || key == "killkey" || key == "key_hot" || key == "hd_hot";
    bool is_hot_id = false;
    for (auto &h : hots) is_hot_id = is_hot_id || (h.first != key && *h.second == id);
    if (plat::is_escape(id)) {
      // cancelled: keep the old key
    } else if (plat::is_clear_key(id) && (key == "hotkey" || key == "key_hot" || key == "hd_hot")) {
      slot->clear();
    } else if (is_hotkey) {
      // hotkeys must all be different, and never a key NYC presses itself
      bool clash = is_hot_id;
      for (auto *pk : presses) clash = clash || *pk == id;
      if (!clash) *slot = id;
    } else {
      // a key NYC presses can be anything except one of NYC's own hotkeys (it would trigger itself)
      bool clash = false;
      for (auto &h : hots) clash = clash || *h.second == id;
      if (!clash) *slot = id;
    }
    name = plat::key_name(*slot);
  }
  CFG.save();
  js("onBind(" + json(key).dump() + "," + json(name).dump() + ")");
}

static void run_mode(Engine &e, const std::string &mode, int burst) {
  if (mode == "Toggle") e.toggle();
  else if (mode == "Hold") e.start(0);
  else { e.stop(); e.start(burst); }  // Burst: every press fires a fresh burst
}

static void on_key(const std::string &id, bool down) {
  if (down) {
    bool binding;
    { std::lock_guard<std::mutex> l(g_state_m); binding = !g_binding.empty(); }
    if (binding) { finish_bind(id); return; }
    if (!g_held.insert(id).second) return;  // key auto-repeat
    std::string hot, kill, mode, khot, kmode, hdhot;
    bool armed, karmed, hdon;
    int burst, kburst;
    {
      std::lock_guard<std::mutex> l(CFG.m);
      hot = CFG.hotkey; kill = CFG.killkey; mode = CFG.mode; armed = CFG.armed; burst = CFG.burst;
      khot = CFG.key_hot; kmode = CFG.key_mode; karmed = CFG.key_on; kburst = CFG.key_burst;
      hdhot = CFG.hd_hot; hdon = CFG.hd_on;
    }
    if (id == kill) {
      ENGINE.stop();
      KENGINE.stop();
      HDENGINE.stop();
      if (armed || karmed || hdon) {
        { std::lock_guard<std::mutex> l(CFG.m); CFG.armed = false; CFG.key_on = false; CFG.hd_on = false; }
        CFG.save();
      }
      js("onKilled()");
      return;
    }
    if (g_picking && plat::is_escape(id)) { g_picking = false; js("onPickCancel()"); return; }
    if (armed && id == hot) run_mode(ENGINE, mode, burst);
    if (karmed && id == khot) run_mode(KENGINE, kmode, kburst);
    if (hdon && id == hdhot) HDENGINE.toggle();
  } else {
    g_held.erase(id);
    std::lock_guard<std::mutex> l(CFG.m);
    if (CFG.mode == "Hold" && id == CFG.hotkey) ENGINE.stop();
    if (CFG.key_mode == "Hold" && id == CFG.key_hot) KENGINE.stop();
  }
}

static void on_pick(int x, int y) {
  if (!g_picking.exchange(false)) return;
  std::string target;
  { std::lock_guard<std::mutex> l(g_state_m); target.swap(g_pick_target); }
  if (target == "hd_heal" || target == "hd_drop") {
    {
      std::lock_guard<std::mutex> l(CFG.m);
      if (target == "hd_heal") { CFG.hd_heal_x = x; CFG.hd_heal_y = y; }
      else { CFG.hd_drop_x = x; CFG.hd_drop_y = y; }
    }
    CFG.save();
    js("onSpot(" + json(target).dump() + "," + std::to_string(x) + "," + std::to_string(y) + ")");
    return;
  }
  { std::lock_guard<std::mutex> l(CFG.m); if (CFG.positions.size() < 500) CFG.positions.emplace_back(x, y); }
  CFG.save();
  js("onPositions(" + positions_json() + ")");
}

static std::atomic<int> g_perm{-1};
static std::atomic<bool> g_ready{false};
static void on_perm(bool ok) {
  g_perm = ok ? 1 : 0;
  js(std::string("onPerm(") + (ok ? "true" : "false") + ")");
}

// ============================================================================
// UI thread helpers
// ============================================================================
static void apply_theme() {
  std::string t; bool top;
  { std::lock_guard<std::mutex> l(CFG.m); t = CFG.theme; top = CFG.on_top; }
  auto &th = THEMES.at(t);
  plat::win_theme(th.light, t == "Transparent", th.border, th.bg);
  plat::win_on_top(top);
}

static fs::path icon_file() { return plat::config_dir() / "icon.img"; }

static void restore_icon() {
  bool custom;
  { std::lock_guard<std::mutex> l(CFG.m); custom = CFG.custom_icon; }
  if (!custom) return;
  std::error_code ec;
  if (!fs::exists(icon_file(), ec) || !plat::apply_icon(icon_file())) {
    { std::lock_guard<std::mutex> l(CFG.m); CFG.custom_icon = false; }
    CFG.save();
  }
}

// ---- auto update: bump NYC_VERSION for every release (tag it v<NYC_VERSION> on GitHub)
#define NYC_VERSION "1.0.10"
#define NYC_REPO "BeaglewareTM/Autoclicker"
static void start_updater() {
  std::thread([] {
    plat::update_cleanup();
    if (!plat::update_fetch(NYC_REPO, NYC_VERSION)) return;
    // never restart in the middle of a macro: wait until both are stopped and no keybind/pick is pending
    while (ENGINE.running || KENGINE.running || HDENGINE.running) std::this_thread::sleep_for(std::chrono::milliseconds(250));
    ENGINE.stop();
    KENGINE.stop();
    HDENGINE.stop();
    plat::stop_hooks();
    CFG.save();
    plat::update_install_and_restart();
  }).detach();
}

[[noreturn]] static void shutdown_now() {
  ENGINE.stop();
  KENGINE.stop();
  HDENGINE.stop();
  plat::stop_hooks();  // free the mouse/keyboard hooks first so the PC never lags
  CFG.save();
  plat::quit_now();
}

static int clamp_height(const json &v) {
  double h = v.is_number() ? v.get<double>() : 500.0;
  if (!std::isfinite(h)) h = 500.0;
  return (int)std::clamp(std::ceil(h), 120.0, 2000.0);
}

using Args = const std::string &;
static json args(Args req) {
  json a = json::parse(req, nullptr, false);
  return a.is_array() ? a : json::array();
}

static void bind_all() {
  W->bind("bc_export_settings", [](Args) {
    fs::path out = plat::choose_export_file();
    if (out.empty()) return std::string(R"({"done":false})");
    std::string text;
    { std::lock_guard<std::mutex> l(CFG.m); json j = CFG.to_json_locked(); j["nyc_settings"] = 1; text = j.dump(1); }
    bool ok = false;
    try {
      std::ofstream o(out, std::ios::binary | std::ios::trunc);
      o << text;
      ok = o.good();
    } catch (...) {}
    return std::string(ok ? R"({"done":true,"ok":true})" : R"({"done":true,"ok":false})");
  });
  W->bind("bc_import_settings", [](Args) {
    fs::path in = plat::choose_import_file();
    if (in.empty()) return std::string(R"({"done":false})");
    Settings fresh;
    if (!fresh.load_from(in)) return std::string(R"({"done":true,"ok":false})");
    ENGINE.stop();
    KENGINE.stop();
    HDENGINE.stop();
    {
      std::lock_guard<std::mutex> l(CFG.m);
      bool icon = CFG.custom_icon;
      CFG.copy_from(fresh);
      CFG.armed = false;   // never come back armed from a file
      CFG.key_on = false;
      CFG.hd_on = false;
      CFG.custom_icon = icon;  // the icon image itself isn't in the file, keep the current one
    }
    CFG.save();
    apply_theme();
    return std::string(R"({"done":true,"ok":true})");
  });
  W->bind("bc_get_state", [](Args) {
    json j;
    { std::lock_guard<std::mutex> l(CFG.m); j = CFG.to_json_locked(); j["hotkey_name"] = plat::key_name(CFG.hotkey);
      j["killkey_name"] = plat::key_name(CFG.killkey);
      j["key_hot_name"] = plat::key_name(CFG.key_hot);
      j["key_target_name"] = plat::key_name(CFG.key_target);
      j["hd_hot_name"] = plat::key_name(CFG.hd_hot);
      j["hd_heal_key_name"] = plat::key_name(CFG.hd_heal_key);
      j["hd_drop_key_name"] = plat::key_name(CFG.hd_drop_key); }
    j["platform"] = plat::name();
    j["version"] = NYC_VERSION;
    int p = g_perm.load();
    if (p >= 0) j["perm"] = p == 1;
    return j.dump();
  });

  W->bind("bc_set", [](Args req) {
    json a = args(req);
    if (a.size() < 2 || !a[0].is_string()) return std::string("null");
    std::string k = a[0];
    const json &v = a[1];
    bool stop = false, kstop = false, hdstop = false, theme = false, top = false;
    {
      std::lock_guard<std::mutex> l(CFG.m);
      try {
        if (k == "armed") { CFG.armed = v.get<bool>(); stop = !CFG.armed; }
        else if (k == "cps") CFG.cps = v.get<double>();
        else if (k == "button") CFG.button = v.get<std::string>();
        else if (k == "ctype") { CFG.ctype = v.get<std::string>(); stop = true; }
        else if (k == "mode") { CFG.mode = v.get<std::string>(); stop = true; }
        else if (k == "pos_delay") CFG.pos_delay = (int)std::lround(v.get<double>());
        else if (k == "burst") CFG.burst = (int)std::lround(v.get<double>());
        else if (k == "theme") { CFG.theme = v.get<std::string>(); theme = true; }
        else if (k == "on_top") { CFG.on_top = v.get<bool>(); top = true; }
        else if (k == "chat_type") CFG.chat_type = v.get<bool>();
        else if (k == "key_on") { CFG.key_on = v.get<bool>(); kstop = !CFG.key_on; }
        else if (k == "key_mode") { CFG.key_mode = v.get<std::string>(); kstop = true; }
        else if (k == "key_delay") CFG.key_delay = v.get<double>();
        else if (k == "key_burst") CFG.key_burst = (int)std::lround(v.get<double>());
        else if (k == "hd_on") { CFG.hd_on = v.get<bool>(); hdstop = !CFG.hd_on; }
        else if (k == "hd_wait") CFG.hd_wait = (int)std::lround(v.get<double>());
      } catch (...) {}
      CFG.sanitize();
    }
    if (stop) ENGINE.stop();
    if (kstop) KENGINE.stop();
    if (hdstop) HDENGINE.stop();
    if (theme || top) apply_theme();
    CFG.save();
    return std::string("null");
  });

  W->bind("bc_ready", [](Args req) {
    json a = args(req);
    plat::win_resize(WIN_W, clamp_height(a.empty() ? json() : a[0]));
    apply_theme();
    if (!g_ready.exchange(true)) { plat::win_center(); restore_icon(); }
    plat::win_show();
    return std::string("true");
  });
  W->bind("bc_resize", [](Args req) {
    json a = args(req);
    plat::win_resize(WIN_W, clamp_height(a.empty() ? json() : a[0]));
    return std::string("null");
  });
  W->bind("bc_minimize", [](Args) { plat::win_minimize(); return std::string("null"); });
  W->bind("bc_close", [](Args) -> std::string { shutdown_now(); return "null"; });
  W->bind("bc_drag_begin", [](Args) { plat::win_drag_begin(); return std::string("null"); });
  W->bind("bc_drag_to", [](Args req) {
    json a = args(req);
    if (a.size() == 2 && a[0].is_number() && a[1].is_number())
      plat::win_drag_to((int)a[0].get<double>(), (int)a[1].get<double>());
    return std::string("null");
  });
  W->bind("bc_start_bind", [](Args req) {
    json a = args(req);
    if (!a.empty() && a[0].is_string() &&
        (a[0] == "hotkey" || a[0] == "killkey" || a[0] == "key_hot" || a[0] == "key_target" ||
         a[0] == "hd_hot" || a[0] == "hd_heal_key" || a[0] == "hd_drop_key")) {
      std::lock_guard<std::mutex> l(g_state_m);
      g_binding = a[0].get<std::string>();
    }
    return std::string("null");
  });
  W->bind("bc_cancel_bind", [](Args) {
    std::lock_guard<std::mutex> l(g_state_m);
    g_binding.clear();
    return std::string("null");
  });
  W->bind("bc_choose_icon", [](Args) {
    fs::path chosen = plat::choose_icon_file();
    if (chosen.empty()) return std::string(R"({"changed":false})");
    std::error_code ec;
    fs::create_directories(plat::config_dir(), ec);
    fs::path tmp = icon_file();
    tmp += ".new";
    if (!fs::copy_file(chosen, tmp, fs::copy_options::overwrite_existing, ec) || !plat::apply_icon(tmp)) {
      fs::remove(tmp, ec);
      restore_icon();
      return std::string(R"({"changed":false,"error":true})");
    }
    fs::rename(tmp, icon_file(), ec);
    plat::apply_icon(icon_file());
    { std::lock_guard<std::mutex> l(CFG.m); CFG.custom_icon = true; }
    CFG.save();
    return std::string(R"({"changed":true})");
  });
  W->bind("bc_reset_icon", [](Args) {
    plat::apply_icon({});
    std::error_code ec;
    fs::remove(icon_file(), ec);
    { std::lock_guard<std::mutex> l(CFG.m); CFG.custom_icon = false; }
    CFG.save();
    return std::string("null");
  });
  W->bind("bc_start_pick", [](Args req) {
    json a = args(req);
    ENGINE.stop();
    KENGINE.stop();
    HDENGINE.stop();
    {
      std::lock_guard<std::mutex> l(g_state_m);
      g_pick_target = (!a.empty() && a[0].is_string() && (a[0] == "hd_heal" || a[0] == "hd_drop")) ? a[0].get<std::string>() : "";
    }
    g_picking = true;
    return std::string("null");
  });
  W->bind("bc_cancel_pick", [](Args) { g_picking = false; return std::string("null"); });
  W->bind("bc_remove_pos", [](Args req) {
    json a = args(req);
    if (!a.empty() && a[0].is_number()) {
      long i = (long)a[0].get<double>();
      std::lock_guard<std::mutex> l(CFG.m);
      if (i >= 0 && i < (long)CFG.positions.size()) CFG.positions.erase(CFG.positions.begin() + i);
    }
    CFG.save();
    js("onPositions(" + positions_json() + ")");
    return std::string("null");
  });
  W->bind("bc_clear_pos", [](Args) {
    { std::lock_guard<std::mutex> l(CFG.m); CFG.positions.clear(); }
    CFG.save();
    js("onPositions([])");
    return std::string("null");
  });
}

// ============================================================================
int main() {
  plat::early_init();
  CFG.load();

  void *native = plat::create_window(WIN_W, 500);
  try {
    static webview::webview w(false, native);
    W = &w;
    void *win = w.window().ok() ? w.window().value() : nullptr;
    void *widget = w.widget().ok() ? w.widget().value() : nullptr;
    plat::attach(win, widget, shutdown_now);
    if (auto c = w.browser_controller(); c.ok()) plat::set_browser(c.value());
    w.set_title("NYC");
    bind_all();
    start_updater();
    w.set_html(std::string(reinterpret_cast<const char *>(UI_HTML), sizeof(UI_HTML)));
    plat::start_hooks(on_key, on_pick, on_perm);
    std::thread([] {
      std::this_thread::sleep_for(std::chrono::seconds(4));
      if (!g_ready && W) W->dispatch([] {
        if (g_ready.exchange(true)) return;
        plat::win_resize(WIN_W, 488);
        apply_theme();
        plat::win_center();
        restore_icon();
        plat::win_show();
      });
    }).detach();
    w.run();
  } catch (const std::exception &e) {
    plat::show_error(std::string("NYC could not start.\n\n") + e.what() +
                     "\n\nMake sure Microsoft Edge WebView2 Runtime is installed:\n"
                     "https://developer.microsoft.com/microsoft-edge/webview2/");
  } catch (...) {
    plat::show_error("NYC could not start.");
  }
  shutdown_now();
}
