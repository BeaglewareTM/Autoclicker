// NYC - macOS platform layer (Objective-C++, manual reference counting)
#import <ApplicationServices/ApplicationServices.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <unistd.h>

#include <mutex>
#include <thread>

#include "platform.h"

namespace {

NSWindow *g_win = nil;
int g_lw = 517, g_lh = 500;
std::function<void()> g_on_close;
NSPoint g_drag_origin;

// Our window's rectangle in global "top-left origin" coordinates (what CGEvent uses),
// so the click thread can skip clicks that would land on our own window.
std::mutex g_rect_m;
CGRect g_rect = CGRectZero;
bool g_rect_visible = false;

plat::KeyFn g_key;
plat::PickFn g_pick;
plat::PermFn g_perm;
CFMachPortRef g_tap = nullptr;
std::atomic<bool> g_tap_ok{false};
std::thread g_tap_thread;
CFRunLoopRef g_tap_loop = nullptr;
CGEventSourceRef g_src = nullptr;

BOOL yes_imp(id, SEL) { return YES; }

void update_rect() {
  if (!g_win) return;
  NSRect f = [g_win frame];
  CGFloat top = NSScreen.screens.count ? NSScreen.screens[0].frame.size.height : 0;
  std::lock_guard<std::mutex> l(g_rect_m);
  g_rect = CGRectMake(f.origin.x, top - (f.origin.y + f.size.height), f.size.width, f.size.height);
  g_rect_visible = [g_win isVisible] && ![g_win isMiniaturized];
}

bool over_self(CGPoint p) {
  std::lock_guard<std::mutex> l(g_rect_m);
  return g_rect_visible && CGRectContainsPoint(g_rect, p);
}

CGPoint cursor() {
  CGEventRef e = CGEventCreate(nullptr);
  CGPoint p = CGEventGetLocation(e);
  CFRelease(e);
  return p;
}

void post_clicks(CGPoint p, int btn, int count) {
  CGEventType down = btn == 0 ? kCGEventLeftMouseDown : btn == 1 ? kCGEventRightMouseDown : kCGEventOtherMouseDown;
  CGEventType up = btn == 0 ? kCGEventLeftMouseUp : btn == 1 ? kCGEventRightMouseUp : kCGEventOtherMouseUp;
  CGMouseButton b = btn == 0 ? kCGMouseButtonLeft : btn == 1 ? kCGMouseButtonRight : kCGMouseButtonCenter;
  for (int i = 1; i <= count; ++i) {
    CGEventRef d = CGEventCreateMouseEvent(g_src, down, p, b);
    CGEventSetIntegerValueField(d, kCGMouseEventClickState, i);
    CGEventPost(kCGHIDEventTap, d);
    CFRelease(d);
    CGEventRef u = CGEventCreateMouseEvent(g_src, up, p, b);
    CGEventSetIntegerValueField(u, kCGMouseEventClickState, i);
    CGEventPost(kCGHIDEventTap, u);
    CFRelease(u);
  }
}

// ---- global event tap ----
CGEventRef tap_cb(CGEventTapProxy, CGEventType type, CGEventRef e, void *) {
  if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
    if (g_tap) CGEventTapEnable(g_tap, true);
    return e;
  }
  if (CGEventGetIntegerValueField(e, kCGEventSourceUnixProcessID) == getpid()) return e;  // our own clicks
  switch (type) {
    case kCGEventKeyDown:
    case kCGEventKeyUp:
      g_key("mk:" + std::to_string(CGEventGetIntegerValueField(e, kCGKeyboardEventKeycode)), type == kCGEventKeyDown);
      break;
    case kCGEventFlagsChanged: {
      int64_t kc = CGEventGetIntegerValueField(e, kCGKeyboardEventKeycode);
      CGEventFlags f = CGEventGetFlags(e);
      CGEventFlags mask = 0;
      switch (kc) {
        case 56: case 60: mask = kCGEventFlagMaskShift; break;
        case 59: case 62: mask = kCGEventFlagMaskControl; break;
        case 58: case 61: mask = kCGEventFlagMaskAlternate; break;
        case 55: case 54: mask = kCGEventFlagMaskCommand; break;
        case 57: mask = kCGEventFlagMaskAlphaShift; break;
        case 63: mask = kCGEventFlagMaskSecondaryFn; break;
      }
      if (mask) g_key("mk:" + std::to_string(kc), (f & mask) != 0);
      break;
    }
    case kCGEventLeftMouseDown: {
      CGPoint p = CGEventGetLocation(e);
      g_pick((int)p.x, (int)p.y);
      break;
    }
    case kCGEventOtherMouseDown:
    case kCGEventOtherMouseUp: {
      int64_t b = CGEventGetIntegerValueField(e, kCGMouseEventButtonNumber);
      const char *id = b == 2 ? "m:middle" : b == 3 ? "m:x1" : b == 4 ? "m:x2" : nullptr;
      if (id) g_key(id, type == kCGEventOtherMouseDown);
      break;
    }
    default:
      break;
  }
  return e;
}

bool try_create_tap() {
  CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) |
                     CGEventMaskBit(kCGEventFlagsChanged) | CGEventMaskBit(kCGEventLeftMouseDown) |
                     CGEventMaskBit(kCGEventOtherMouseDown) | CGEventMaskBit(kCGEventOtherMouseUp);
  g_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly, mask, tap_cb,
                           nullptr);
  return g_tap != nullptr;
}

bool accessibility_ok(bool prompt) {
  NSDictionary *opts = @{(__bridge id)kAXTrustedCheckOptionPrompt : @(prompt)};
  return AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)opts);
}

}  // namespace

namespace plat {

const char *name() { return "mac"; }

void early_init() {
  g_src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
  // Stop App Nap from throttling NYC in the background (hotkeys and clicking would slow to a crawl)
  static id activity = [[[NSProcessInfo processInfo]
      beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical
                        reason:@"Autoclicker and hotkeys"] retain];
  (void)activity;
}

void *create_window(int lw, int lh) {
  g_lw = lw;
  g_lh = lh;
  return nullptr;  // the webview library creates the NSWindow; attach() restyles it
}

void attach(void *window, void *, std::function<void()> on_close) {
  g_win = (NSWindow *)window;
  g_on_close = std::move(on_close);
  if (!g_win) return;
  [g_win orderOut:nil];

  // Borderless windows cannot take keyboard focus by default: swap in a tiny subclass that can.
  Class base = [g_win class];
  Class cls = objc_lookUpClass("BeagleWindow");
  if (!cls) {
    cls = objc_allocateClassPair(base, "BeagleWindow", 0);
    class_addMethod(cls, @selector(canBecomeKeyWindow), (IMP)yes_imp, "c@:");
    class_addMethod(cls, @selector(canBecomeMainWindow), (IMP)yes_imp, "c@:");
    objc_registerClassPair(cls);
  }
  object_setClass(g_win, cls);

  [g_win setStyleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskMiniaturizable];
  [g_win setOpaque:NO];
  [g_win setBackgroundColor:[NSColor clearColor]];
  [g_win setHasShadow:YES];
  [g_win setMovable:NO];  // we move it ourselves from the custom title bar
  NSView *cv = [g_win contentView];
  [cv setWantsLayer:YES];
  cv.layer.cornerRadius = 12.0;
  cv.layer.masksToBounds = YES;
  if (@available(macOS 10.15, *)) cv.layer.cornerCurve = kCACornerCurveContinuous;

  NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
  for (NSNotificationName n in @[ NSWindowDidDeminiaturizeNotification, NSWindowDidMiniaturizeNotification,
                                  NSWindowDidMoveNotification, NSWindowDidResizeNotification ])
    [nc addObserverForName:n object:g_win queue:nil usingBlock:^(NSNotification *note) { (void)note; update_rect(); }];
}

void set_browser(void *) {}

void win_resize(int lw, int lh) {
  g_lw = lw;
  g_lh = lh;
  NSRect f = [g_win frame];
  CGFloat top = f.origin.y + f.size.height;  // keep the top edge where it is
  [g_win setFrame:NSMakeRect(f.origin.x, top - lh, lw, lh) display:YES];
  [g_win invalidateShadow];
  update_rect();
}

void win_center() {
  NSPoint m = [NSEvent mouseLocation];
  NSScreen *scr = [NSScreen mainScreen];
  for (NSScreen *s in [NSScreen screens])
    if (NSPointInRect(m, s.frame)) { scr = s; break; }
  NSRect a = scr.visibleFrame;
  NSRect f = [g_win frame];
  [g_win setFrameOrigin:NSMakePoint(a.origin.x + (a.size.width - f.size.width) / 2,
                                    a.origin.y + (a.size.height - f.size.height) / 2)];
  update_rect();
}

void win_show() {
  [NSApp activateIgnoringOtherApps:YES];
  [g_win makeKeyAndOrderFront:nil];
  [g_win invalidateShadow];
  update_rect();
}

void win_minimize() {
  [g_win miniaturize:nil];
  update_rect();
}

void win_on_top(bool on) { [g_win setLevel:on ? NSFloatingWindowLevel : NSNormalWindowLevel]; }

void win_theme(bool light, bool translucent, unsigned, unsigned bg) {
  [g_win setBackgroundColor:[NSColor colorWithSRGBRed:((bg >> 16) & 255) / 255.0 green:((bg >> 8) & 255) / 255.0 blue:(bg & 255) / 255.0 alpha:1.0]];
  [g_win setAppearance:[NSAppearance appearanceNamed:light ? NSAppearanceNameAqua : NSAppearanceNameDarkAqua]];
  [g_win setAlphaValue:translucent ? 0.87 : 1.0];
}

void win_drag_begin() { g_drag_origin = [g_win frame].origin; }

void win_drag_to(int dx, int dy) {
  [g_win setFrameOrigin:NSMakePoint(g_drag_origin.x + dx, g_drag_origin.y - dy)];
  update_rect();
}

void start_hooks(KeyFn on_key, PickFn on_pick, PermFn on_perm) {
  g_key = std::move(on_key);
  g_pick = std::move(on_pick);
  g_perm = std::move(on_perm);
  accessibility_ok(true);  // shows the system prompt the first time (needed to send clicks)
  if (@available(macOS 10.15, *)) CGRequestListenEventAccess();  // Input Monitoring (needed for hotkeys)
  g_tap_thread = std::thread([] {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    // Keep trying until the user grants permission in System Settings
    bool reported = false;
    while (!try_create_tap() || !accessibility_ok(false)) {
      if (g_tap) { CFRelease(g_tap); g_tap = nullptr; }
      if (!reported) { g_perm(false); reported = true; }
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    g_tap_ok = true;
    g_perm(true);
    CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, g_tap, 0);
    g_tap_loop = CFRunLoopGetCurrent();
    CFRunLoopAddSource(g_tap_loop, src, kCFRunLoopCommonModes);
    CGEventTapEnable(g_tap, true);
    CFRunLoopRun();
    CFRelease(src);
  });
  g_tap_thread.detach();
}

void stop_hooks() {
  if (g_tap) {
    CGEventTapEnable(g_tap, false);
    CFMachPortInvalidate(g_tap);
  }
  if (g_tap_loop) CFRunLoopStop(g_tap_loop);
}

void click(int btn, bool dbl) {
  CGPoint p = cursor();
  if (over_self(p)) return;
  post_clicks(p, btn, dbl ? 2 : 1);
}

void click_at(int x, int y, int btn) {
  CGPoint p = CGPointMake(x, y);
  if (over_self(p)) return;
  CGEventRef mv = CGEventCreateMouseEvent(g_src, kCGEventMouseMoved, p, kCGMouseButtonLeft);
  CGEventPost(kCGHIDEventTap, mv);
  CFRelease(mv);
  post_clicks(p, btn, 1);
}

void press_key(const std::string &id) {
  if ([[NSRunningApplication currentApplication] isActive]) return;  // never type into NYC itself
  if (id.rfind("m:", 0) == 0) {
    int b = id == "m:middle" ? 2 : id == "m:x1" ? 3 : 4;
    CGPoint p = cursor();
    for (int i = 0; i < 2; ++i) {
      CGEventRef e = CGEventCreateMouseEvent(g_src, i ? kCGEventOtherMouseUp : kCGEventOtherMouseDown, p,
                                             (CGMouseButton)b);
      CGEventSetIntegerValueField(e, kCGMouseEventButtonNumber, b);
      CGEventPost(kCGHIDEventTap, e);
      CFRelease(e);
    }
    return;
  }
  if (id.rfind("mk:", 0) != 0) return;
  CGKeyCode kc = (CGKeyCode)atoi(id.c_str() + 3);
  for (int i = 0; i < 2; ++i) {
    CGEventRef e = CGEventCreateKeyboardEvent(g_src, kc, i == 0);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
  }
}

void engine_thread_begin() { pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0); }
void engine_thread_end() {}

bool wait_until(Clock::time_point t, const std::atomic<bool> &running) {
  using namespace std::chrono;
  for (;;) {
    auto rem = t - Clock::now();
    if (rem <= Clock::duration::zero()) return true;
    if (!running) return false;
    if (rem > microseconds(500)) {
      std::this_thread::sleep_for(std::min<Clock::duration>(rem - microseconds(300), milliseconds(10)));
    } else {
      std::this_thread::yield();
    }
  }
}

std::string key_name(const std::string &id) {
  if (id.empty()) return "Not set";
  if (id == "m:middle") return "Mouse 3";
  if (id == "m:x1") return "Mouse 4";
  if (id == "m:x2") return "Mouse 5";
  if (id.rfind("mk:", 0) != 0) return id;
  static const std::pair<int, const char *> names[] = {
      {0, "A"}, {11, "B"}, {8, "C"}, {2, "D"}, {14, "E"}, {3, "F"}, {5, "G"}, {4, "H"}, {34, "I"}, {38, "J"},
      {40, "K"}, {37, "L"}, {46, "M"}, {45, "N"}, {31, "O"}, {35, "P"}, {12, "Q"}, {15, "R"}, {1, "S"},
      {17, "T"}, {32, "U"}, {9, "V"}, {13, "W"}, {7, "X"}, {16, "Y"}, {6, "Z"}, {29, "0"}, {18, "1"},
      {19, "2"}, {20, "3"}, {21, "4"}, {23, "5"}, {22, "6"}, {26, "7"}, {28, "8"}, {25, "9"},
      {122, "F1"}, {120, "F2"}, {99, "F3"}, {118, "F4"}, {96, "F5"}, {97, "F6"}, {98, "F7"}, {100, "F8"},
      {101, "F9"}, {109, "F10"}, {103, "F11"}, {111, "F12"}, {105, "F13"}, {107, "F14"}, {113, "F15"},
      {106, "F16"}, {64, "F17"}, {79, "F18"}, {80, "F19"}, {90, "F20"},
      {36, "Return"}, {48, "Tab"}, {49, "Space"}, {51, "Delete"}, {53, "Esc"}, {117, "Forward Delete"},
      {114, "Insert"}, {115, "Home"}, {119, "End"}, {116, "Page Up"}, {121, "Page Down"},
      {123, "Left"}, {124, "Right"}, {125, "Down"}, {126, "Up"},
      {55, "Command"}, {54, "Right Command"}, {56, "Shift"}, {60, "Right Shift"}, {58, "Option"},
      {61, "Right Option"}, {59, "Control"}, {62, "Right Control"}, {57, "Caps Lock"}, {63, "Fn"},
      {24, "="}, {27, "-"}, {30, "]"}, {33, "["}, {39, "'"}, {41, ";"}, {42, "\\"}, {43, ","}, {44, "/"},
      {47, "."}, {50, "`"}, {65, "Num ."}, {67, "Num *"}, {69, "Num +"}, {71, "Clear"}, {75, "Num /"},
      {76, "Num Enter"}, {78, "Num -"}, {81, "Num ="}, {82, "Num 0"}, {83, "Num 1"}, {84, "Num 2"},
      {85, "Num 3"}, {86, "Num 4"}, {87, "Num 5"}, {88, "Num 6"}, {89, "Num 7"}, {91, "Num 8"}, {92, "Num 9"}};
  int kc = atoi(id.c_str() + 3);
  for (auto &n : names)
    if (n.first == kc) return n.second;
  return "Key " + std::to_string(kc);
}

std::string default_hotkey() { return "mk:97"; }   // F6
std::string default_killkey() { return "mk:114"; } // Insert (Help) key
std::string default_keyhot() { return "mk:98"; }   // F7
std::string default_hdhot() { return "mk:100"; }    // F8
std::string default_heal_key() { return "mk:8"; }   // C
std::string default_drop_key() { return "mk:6"; }   // Z
bool is_escape(const std::string &id) { return id == "mk:53"; }
bool is_clear_key(const std::string &id) { return id == "mk:51" || id == "mk:117"; }

std::filesystem::path config_dir() {
  const char *home = getenv("HOME");
  std::filesystem::path base = home ? home : "/tmp";
  return base / "Library" / "Application Support" / "BeagleAutoclicker";
}

void show_error(const std::string &msg) {
  NSAlert *a = [[NSAlert alloc] init];
  [a setMessageText:@"NYC"];
  [a setInformativeText:[NSString stringWithUTF8String:msg.c_str()]];
  [a runModal];
  [a release];
}

std::filesystem::path choose_icon_file() {
  NSOpenPanel *p = [NSOpenPanel openPanel];
  [p setTitle:@"Choose an icon image"];
  [p setCanChooseDirectories:NO];
  [p setAllowsMultipleSelection:NO];
  [p setAllowedFileTypes:@[ @"png", @"jpg", @"jpeg", @"ico", @"icns", @"gif", @"bmp", @"tiff", @"webp", @"heic" ]];
  if ([p runModal] != NSModalResponseOK || p.URLs.count == 0) return {};
  return std::filesystem::path([p.URLs[0].path UTF8String]);
}

// Type while clicking is Windows-only for now.
void chat_relay(bool, const std::vector<std::string> &) {}
bool chat_flush() { return false; }

// Auto update is Windows-only for now.
bool update_fetch(const char *, const char *) { return false; }
void update_install_and_restart() { quit_now(); }
void update_cleanup() {}

std::filesystem::path choose_export_file() {
  NSSavePanel *p = [NSSavePanel savePanel];
  [p setTitle:@"Export settings"];
  [p setNameFieldStringValue:@"NYC settings.json"];
  [p setAllowedFileTypes:@[ @"json" ]];
  if ([p runModal] != NSModalResponseOK || !p.URL) return {};
  return std::filesystem::path([p.URL.path UTF8String]);
}

std::filesystem::path choose_import_file() {
  NSOpenPanel *p = [NSOpenPanel openPanel];
  [p setTitle:@"Import settings"];
  [p setCanChooseDirectories:NO];
  [p setAllowsMultipleSelection:NO];
  [p setAllowedFileTypes:@[ @"json" ]];
  if ([p runModal] != NSModalResponseOK || p.URLs.count == 0) return {};
  return std::filesystem::path([p.URLs[0].path UTF8String]);
}

bool apply_icon(const std::filesystem::path &image) {
  if (image.empty()) {
    [NSApp setApplicationIconImage:nil];  // back to the app bundle's icon
    return true;
  }
  NSImage *img = [[NSImage alloc] initWithContentsOfFile:[NSString stringWithUTF8String:image.c_str()]];
  if (!img || img.size.width <= 0) {
    [img release];
    return false;
  }
  [NSApp setApplicationIconImage:img];
  [img release];
  return true;
}

void quit_now() {
  if (g_win) [g_win orderOut:nil];
  _exit(0);
}

}  // namespace plat
