#define main beagle_main
#include "../src/main.cpp"
#undef main
#include <cassert>
#include <cstdio>
extern std::atomic<long> CLICKS; extern std::atomic<long> AT; extern std::atomic<long> KEYS;
using namespace std::chrono_literals;
static webview::webview *M;
static void key(const char *k, bool d) { on_key(k, d); }
static void tap(const char *k) { key(k, true); key(k, false); }
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fflush(stdout); std::_Exit(1); } } while (0)
int main() {
  std::filesystem::remove_all("/tmp/beagle_test_cfg");
  CFG.load();
  static webview::webview w(false, nullptr); W = &w; M = &w; bind_all();
  // state + sanitizing
  auto st = json::parse(M->call("bc_get_state", "[]")); printf("%s\n", st.dump().c_str());
  if (st.value("version", "") != "1.0.2") { printf("FAIL version missing\n"); return 1; }
  CHECK(st["cps"] == 20.0 && st["hotkey_name"] == "F6" && st["killkey_name"] == "Insert" && st["theme"] == "Dark");
  M->call("bc_set", R"(["cps", 99999])"); CHECK(CFG.cps == 1000);
  M->call("bc_set", R"(["cps", "garbage"])"); CHECK(CFG.cps == 1000);
  M->call("bc_set", R"(["button", "Bogus"])"); CHECK(CFG.button == "Left");
  M->call("bc_set", R"(["theme", "Nope"])"); CHECK(CFG.theme == "Dark");
  M->call("bc_set", R"(["theme", "Ocean"])"); M->call("bc_set", R"(["armed", true])");
  // Not armed -> hotkey ignored
  M->call("bc_set", R"(["armed", false])"); tap("vk:117"); std::this_thread::sleep_for(50ms); CHECK(!ENGINE.running);
  M->call("bc_set", R"(["armed", true])");
  // Toggle at 1000 cps for 1 s
  CLICKS = 0; tap("vk:117"); std::this_thread::sleep_for(1000ms); tap("vk:117"); std::this_thread::sleep_for(30ms);
  printf("toggle 1000cps 1s: %ld clicks\n", CLICKS.load()); CHECK(CLICKS >= 970 && CLICKS <= 1010); CHECK(!ENGINE.running);
  // auto-repeat must not toggle off
  M->call("bc_set", R"(["cps", 50])");
  key("vk:117", true); key("vk:117", true); key("vk:117", true); std::this_thread::sleep_for(50ms); CHECK(ENGINE.running);
  key("vk:117", false);
  // killswitch disarms
  tap("vk:45"); std::this_thread::sleep_for(30ms); printf("last=%s armed=%d run=%d\n", M->last().c_str(), (int)CFG.armed, (int)ENGINE.running.load()); CHECK(!ENGINE.running && !CFG.armed);
  // burst
  M->call("bc_set", R"(["armed", true])"); M->call("bc_set", R"(["mode", "Burst"])"); M->call("bc_set", R"(["burst", 25])");
  CLICKS = 0; tap("vk:117"); std::this_thread::sleep_for(800ms); printf("burst: %ld\n", CLICKS.load()); CHECK(CLICKS == 25 && !ENGINE.running);
  // burst re-press mid burst restarts
  M->call("bc_set", R"(["burst", 1000])"); CLICKS = 0; tap("vk:117"); std::this_thread::sleep_for(100ms); tap("vk:117");
  std::this_thread::sleep_for(100ms); CHECK(ENGINE.running); tap("vk:45");
  // hold
  M->call("bc_set", R"(["armed", true])"); M->call("bc_set", R"(["mode", "Hold"])"); CLICKS = 0;
  key("vk:117", true); std::this_thread::sleep_for(300ms); key("vk:117", false); long c = CLICKS; std::this_thread::sleep_for(100ms);
  printf("hold 0.3s @50: %ld\n", c); CHECK(c >= 13 && c <= 18 && CLICKS == c);
  // double click counts 2
  M->call("bc_set", R"(["mode", "Burst"])"); M->call("bc_set", R"(["burst", 5])"); M->call("bc_set", R"(["ctype", "Double"])");
  CLICKS = 0; tap("vk:117"); std::this_thread::sleep_for(300ms); CHECK(CLICKS == 10);
  // multi with positions
  M->call("bc_start_pick", "[]"); on_pick(10, 20); on_pick(30, 40); M->call("bc_start_pick", "[]"); on_pick(50, 60);
  CHECK(CFG.positions.size() == 2);  // second on_pick ignored (not picking)
  M->call("bc_set", R"(["ctype", "Multi"])"); M->call("bc_set", R"(["pos_delay", 0])"); M->call("bc_set", R"(["burst", 3])");
  AT = 0; tap("vk:117"); std::this_thread::sleep_for(300ms); CHECK(AT == 6);
  M->call("bc_remove_pos", "[0]"); CHECK(CFG.positions.size() == 1 && CFG.positions[0].first == 50);
  // binding
  M->call("bc_start_bind", R"(["hotkey"])"); tap("vk:71"); CHECK(CFG.hotkey == "vk:71");
  M->call("bc_start_bind", R"(["killkey"])"); tap("vk:71"); CHECK(CFG.killkey == "vk:45");  // same as hotkey: refused
  M->call("bc_start_bind", R"(["hotkey"])"); tap("vk:27"); CHECK(CFG.hotkey == "vk:71");   // Esc cancels
  M->call("bc_start_bind", R"(["hotkey"])"); tap("vk:8"); CHECK(CFG.hotkey.empty());      // Backspace clears
  // persistence
  M->call("bc_start_bind", R"(["hotkey"])"); tap("vk:117");
  Settings s2; s2.load();
  printf("reloaded: cps=%.1f mode=%s ctype=%s theme=%s burst=%d pos=%zu hotkey=%s\n", s2.cps, s2.mode.c_str(), s2.ctype.c_str(), s2.theme.c_str(), s2.burst, s2.positions.size(), s2.hotkey.c_str());
  CHECK(s2.cps == 50 && s2.mode == "Burst" && s2.ctype == "Multi" && s2.theme == "Ocean" && s2.burst == 3 && s2.positions.size() == 1 && s2.hotkey == "vk:117");
  // corrupt file -> defaults, no crash
  { std::ofstream o("/tmp/beagle_test_cfg/settings.json"); o << "{garbage"; }
  Settings s3; s3.load(); CHECK(s3.cps == 20.0 && s3.hotkey == "vk:117");
  { std::ofstream o("/tmp/beagle_test_cfg/settings.json"); o << R"({"cps":"x","positions":[[1],[2,3],"a"],"burst":-5})"; }
  Settings s4; s4.load(); CHECK(s4.cps == 20.0 && s4.positions.size() == 1 && s4.burst == 1);
  // ---------- key macro ----------
  { std::ofstream o("/tmp/beagle_test_cfg/settings.json"); o << "{}"; }
  CFG.load();
  st = json::parse(M->call("bc_get_state", "[]"));
  CHECK(st["key_hot_name"] == "vk:118" && st["key_target_name"] == "Not set" && st["key_delay"] == 100.0);
  M->call("bc_set", R"(["key_on", true])");
  tap("vk:118"); std::this_thread::sleep_for(50ms); CHECK(!KENGINE.running);  // no target key yet: does nothing
  M->call("bc_start_bind", R"(["key_target"])"); tap("vk:69"); CHECK(CFG.key_target == "vk:69");
  M->call("bc_set", R"(["key_delay", 10])");
  KEYS = 0; tap("vk:118"); std::this_thread::sleep_for(500ms); tap("vk:118"); std::this_thread::sleep_for(30ms);
  printf("key macro 10ms for 0.5s: %ld presses\n", KEYS.load()); CHECK(KEYS >= 45 && KEYS <= 52 && !KENGINE.running);
  M->call("bc_set", R"(["key_delay", 0.001])"); CHECK(CFG.key_delay == 0.01);   // lowest is 0.01 ms
  M->call("bc_set", R"(["key_delay", 2.345])"); CHECK(std::abs(CFG.key_delay - 2.35) < 1e-9 || std::abs(CFG.key_delay - 2.34) < 1e-9);
  M->call("bc_set", R"(["key_delay", 0.5])");
  KEYS = 0; tap("vk:118"); std::this_thread::sleep_for(200ms); tap("vk:118"); std::this_thread::sleep_for(30ms);
  printf("key macro 0.5ms (capped) for 0.2s: %ld presses\n", KEYS.load()); CHECK(KEYS >= 17 && KEYS <= 22);
  M->call("bc_set", R"(["key_delay", 5])"); M->call("bc_set", R"(["key_mode", "Burst"])"); M->call("bc_set", R"(["key_burst", 7])");
  KEYS = 0; tap("vk:118"); std::this_thread::sleep_for(200ms); printf("key burst: %ld\n", KEYS.load()); CHECK(KEYS == 7 && !KENGINE.running);
  M->call("bc_set", R"(["key_mode", "Hold"])"); KEYS = 0;
  key("vk:118", true); std::this_thread::sleep_for(100ms); key("vk:118", false); long kh = KEYS; std::this_thread::sleep_for(60ms);
  CHECK(kh >= 8 && kh <= 12 && KEYS == kh);
  // both macros at once, then killswitch stops and disarms both
  M->call("bc_set", R"(["key_mode", "Toggle"])"); M->call("bc_set", R"(["armed", true])"); M->call("bc_set", R"(["mode", "Toggle"])");
  M->call("bc_set", R"(["ctype", "Single"])");
  tap("vk:117"); tap("vk:118"); std::this_thread::sleep_for(60ms); CHECK(ENGINE.running && KENGINE.running);
  tap("vk:45"); std::this_thread::sleep_for(40ms); CHECK(!ENGINE.running && !KENGINE.running && !CFG.armed && !CFG.key_on);
  // hotkeys can't clash with each other
  M->call("bc_start_bind", R"(["key_hot"])"); tap("vk:117"); CHECK(CFG.key_hot == "vk:118");  // F6 already clicker hotkey
  M->call("bc_start_bind", R"(["key_hot"])"); tap("vk:119"); CHECK(CFG.key_hot == "vk:119");
  M->call("bc_start_bind", R"(["key_target"])"); tap("vk:8"); CHECK(CFG.key_target == "vk:8");  // Backspace is a valid key to spam
  M->call("bc_start_bind", R"(["key_target"])"); tap("vk:117"); CHECK(CFG.key_target == "vk:8");  // can't spam a hotkey
  M->call("bc_start_bind", R"(["key_target"])"); tap("vk:69"); CHECK(CFG.key_target == "vk:69");
  M->call("bc_start_bind", R"(["hotkey"])"); tap("vk:69"); CHECK(CFG.hotkey == "vk:117");  // hotkey can't be the spam key
  M->call("bc_start_bind", R"(["key_target"])"); tap("vk:8"); CHECK(CFG.key_target == "vk:8");
  Settings s5; s5.load();
  CHECK(s5.key_hot == "vk:119" && s5.key_target == "vk:8" && s5.key_mode == "Toggle" && s5.key_delay == 5 && s5.key_burst == 7);
  printf("key macro settings reload ok\n");
  // bad args never crash
  M->call("bc_set", "garbage"); M->call("bc_set", "[]"); M->call("bc_remove_pos", R"(["x"])"); M->call("bc_resize", R"([null])");
  M->call("bc_drag_to", R"([1])"); M->call("bc_start_bind", R"(["nope"])"); M->call("bc_ready", R"(["1e309"])");
  puts("ALL TESTS PASSED"); fflush(stdout);
  std::_Exit(0);
}
