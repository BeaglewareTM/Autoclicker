// NYC - Windows platform layer
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <propsys.h>
#include <shobjidl.h>
#include <vector>
#include "json.hpp"
#include <dwmapi.h>
#include <commdlg.h>
#include <wincodec.h>
#include <shlobj.h>
#include <immintrin.h>

#include <mutex>
#include <thread>

#include "platform.h"
#include "WebView2.h"

template <class F> static F proc(const wchar_t *mod, const char *fn) {
  return reinterpret_cast<F>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(mod), fn)));
}

namespace {

HWND g_win = nullptr, g_widget = nullptr;
int g_lw = 517, g_lh = 500;
std::function<void()> g_on_close;
HBRUSH g_bg = nullptr;
POINT g_drag_start{};
int g_drag_dpi = 96;
ICoreWebView2Controller2 *g_ctrl2 = nullptr;
HICON g_icon_big = nullptr, g_icon_small = nullptr;  // custom icons we created (destroyed on change)

// Decode any image Windows understands (PNG, JPG, BMP, GIF, ICO...), crop it to a centered square
// and turn it into an icon of the given size.
HICON icon_from_image(const std::filesystem::path &file, int size) {
  IWICImagingFactory *f = nullptr;
  IWICBitmapDecoder *dec = nullptr;
  IWICBitmapFrameDecode *frame = nullptr;
  IWICBitmapClipper *clip = nullptr;
  IWICBitmapScaler *scale = nullptr;
  IWICFormatConverter *conv = nullptr;
  HICON icon = nullptr;
  UINT w = 0, h = 0;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) goto done;
  if (FAILED(f->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) goto done;
  if (FAILED(dec->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) || !w || !h) goto done;
  {
    UINT s = std::min(w, h);
    WICRect r{(INT)((w - s) / 2), (INT)((h - s) / 2), (INT)s, (INT)s};
    if (FAILED(f->CreateBitmapClipper(&clip)) || FAILED(clip->Initialize(frame, &r))) goto done;
    if (FAILED(f->CreateBitmapScaler(&scale)) ||
        FAILED(scale->Initialize(clip, size, size, WICBitmapInterpolationModeFant))) goto done;
    if (FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(scale, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom))) goto done;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    void *bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO *>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) goto done;
    if (SUCCEEDED(conv->CopyPixels(nullptr, size * 4, size * size * 4, static_cast<BYTE *>(bits)))) {
      std::vector<BYTE> zeros(((size + 15) / 16) * 2 * size, 0);
      HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros.data());
      ICONINFO ii{TRUE, 0, 0, mask, color};
      icon = CreateIconIndirect(&ii);
      DeleteObject(mask);
    }
    DeleteObject(color);
  }
done:
  if (conv) conv->Release();
  if (scale) scale->Release();
  if (clip) clip->Release();
  if (frame) frame->Release();
  if (dec) dec->Release();
  if (f) f->Release();
  return icon;
}

// The browser engine paints white until the page draws; make that "blank" colour the theme colour instead.
void set_browser_bg(unsigned bg) {
  if (!g_ctrl2) return;
  COREWEBVIEW2_COLOR c{255, (BYTE)((bg >> 16) & 255), (BYTE)((bg >> 8) & 255), (BYTE)(bg & 255)};
  g_ctrl2->put_DefaultBackgroundColor(c);
}

// ---- hooks ----
std::thread g_hook_thread;
plat::KeyFn g_key;
plat::PickFn g_pick;

// ---- timer resolution ----
using NtSetTimerResolution_t = LONG(NTAPI *)(ULONG, BOOLEAN, PULONG);
using NtQueryTimerResolution_t = LONG(NTAPI *)(PULONG, PULONG, PULONG);
NtSetTimerResolution_t pNtSet = nullptr;
ULONG g_best_res = 0;
std::mutex g_timer_m;
int g_timer_users = 0;

int dpi_of(HWND h) {
  using GetDpiForWindow_t = UINT(WINAPI *)(HWND);
  static auto fn = proc<GetDpiForWindow_t>(L"user32.dll", "GetDpiForWindow");
  UINT d = fn && h ? fn(h) : 96;
  return d ? (int)d : 96;
}

void fit_widget() {
  if (!g_win || !g_widget) return;
  RECT r;
  GetClientRect(g_win, &r);
  MoveWindow(g_widget, 0, 0, r.right - r.left, r.bottom - r.top, TRUE);
}

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SIZE:
      fit_widget();
      return 0;
    case WM_ERASEBKGND: {
      RECT r;
      GetClientRect(h, &r);
      FillRect((HDC)wp, &r, g_bg);
      return 1;
    }
    case WM_DPICHANGED: {
      auto *s = reinterpret_cast<RECT *>(lp);
      int dpi = HIWORD(wp);
      SetWindowPos(h, nullptr, s->left, s->top, MulDiv(g_lw, dpi, 96), MulDiv(g_lh, dpi, 96),
                   SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_CLOSE:
      if (g_on_close) g_on_close();
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

void set_dwm_int(HWND h, DWORD attr, int v) { DwmSetWindowAttribute(h, attr, &v, sizeof(v)); }

// ---- clicking ----
const DWORD DOWN[3] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN};
const DWORD UP[3] = {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP};

struct Packet { INPUT in[5]; UINT n; };
Packet g_packets[3][2];  // [button][double]

void build_packets() {
  for (int b = 0; b < 3; ++b)
    for (int d = 0; d < 2; ++d) {
      Packet &p = g_packets[b][d];
      ZeroMemory(&p, sizeof(p));
      p.n = 0;
      // A zero-distance move first: Roblox and many games only register a click after "hover".
      p.in[p.n].type = INPUT_MOUSE; p.in[p.n++].mi.dwFlags = MOUSEEVENTF_MOVE;
      for (int c = 0; c < (d ? 2 : 1); ++c) {
        p.in[p.n].type = INPUT_MOUSE; p.in[p.n++].mi.dwFlags = DOWN[b];
        p.in[p.n].type = INPUT_MOUSE; p.in[p.n++].mi.dwFlags = UP[b];
      }
    }
}

bool over_self(POINT pt) {
  if (!g_win || !IsWindowVisible(g_win) || IsIconic(g_win)) return false;
  HWND h = WindowFromPoint(pt);
  return h && GetAncestor(h, GA_ROOT) == g_win;
}

// High-resolution waitable timer per click thread (Windows 10 1803+), plain Sleep otherwise.
thread_local HANDLE t_timer = nullptr;

void sleep_for_100ns(LONGLONG units) {
  if (t_timer) {
    LARGE_INTEGER due;
    due.QuadPart = -units;
    if (SetWaitableTimer(t_timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(t_timer, INFINITE);
      return;
    }
  }
  Sleep((DWORD)std::max<LONGLONG>(1, units / 10000));
}

}  // namespace

namespace plat {

const char *name() { return "win"; }

void early_init() {
  // Required before WebView2 can start (the webview library only does this when it owns the window)
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  using SetCtx_t = BOOL(WINAPI *)(HANDLE);
  if (auto fn = proc<SetCtx_t>(L"user32.dll", "SetProcessDpiAwarenessContext"))
    fn((HANDLE)-4);  // per-monitor v2: crisp at every scale
  SetCurrentProcessExplicitAppUserModelID(L"Beagle.Autoclicker");

  // Windows 11 ignores timer requests from background apps unless they opt out of throttling.
  // Opt out of Windows 11 Efficiency mode (EcoQoS, 0x1) and timer throttling (0x4). Without this,
  // Windows slows a background app down after a few minutes and the hotkeys/clicker seem to stop.
  struct PPTS { ULONG Version, ControlMask, StateMask; } pt{1, 0x1 | 0x4, 0};
  using SetProcInfo_t = BOOL(WINAPI *)(HANDLE, int, LPVOID, DWORD);
  if (auto fn = proc<SetProcInfo_t>(L"kernel32.dll", "SetProcessInformation"))
    fn(GetCurrentProcess(), 4 /*ProcessPowerThrottling*/, &pt, sizeof(pt));

  pNtSet = proc<NtSetTimerResolution_t>(L"ntdll.dll", "NtSetTimerResolution");
  auto pNtQuery = proc<NtQueryTimerResolution_t>(L"ntdll.dll", "NtQueryTimerResolution");
  ULONG mn = 0, mx = 0, cur = 0;
  if (pNtQuery && pNtQuery(&mn, &mx, &cur) == 0) g_best_res = mx;  // finest possible, usually 0.5 ms

  build_packets();
}

void *create_window(int lw, int lh) {
  g_lw = lw;
  g_lh = lh;
  HINSTANCE inst = GetModuleHandleW(nullptr);
  g_bg = CreateSolidBrush(RGB(8, 8, 8));
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = inst;
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  wc.hIconSm = wc.hIcon;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"BeagleAutoclickerWindow";
  RegisterClassExW(&wc);
  g_win = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"NYC",
                          WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT,
                          CW_USEDEFAULT, lw, lh, nullptr, nullptr, inst, nullptr);
  // Native Windows 11 rounded corners, shadow and dark frame
  set_dwm_int(g_win, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, 2 /*ROUND*/);
  set_dwm_int(g_win, 2 /*DWMWA_NCRENDERING_POLICY*/, 2 /*ENABLED*/);
  set_dwm_int(g_win, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, 1);
  // No open/minimize/restore animation: on a custom window it shows as a blank flash
  set_dwm_int(g_win, 3 /*DWMWA_TRANSITIONS_FORCEDISABLED*/, 1);
  MARGINS m{1, 1, 1, 1};
  DwmExtendFrameIntoClientArea(g_win, &m);
  return g_win;
}

void attach(void *window, void *widget, std::function<void()> on_close) {
  if (window) g_win = (HWND)window;
  g_widget = (HWND)widget;
  g_on_close = std::move(on_close);
  fit_widget();
}

void set_browser(void *controller) {
  auto *c = static_cast<ICoreWebView2Controller *>(controller);
  if (c && !g_ctrl2) c->QueryInterface(IID_ICoreWebView2Controller2, reinterpret_cast<void **>(&g_ctrl2));
  set_browser_bg(0x080808);
}

void win_resize(int lw, int lh) {
  g_lw = lw;
  g_lh = lh;
  int dpi = dpi_of(g_win);
  SetWindowPos(g_win, nullptr, 0, 0, MulDiv(lw, dpi, 96), MulDiv(lh, dpi, 96),
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void win_center() {
  POINT c;
  GetCursorPos(&c);
  HMONITOR mon = MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(mon, &mi);
  RECT wr;
  GetWindowRect(g_win, &wr);
  int w = wr.right - wr.left, h = wr.bottom - wr.top;
  const RECT &a = mi.rcWork;
  SetWindowPos(g_win, nullptr, a.left + (a.right - a.left - w) / 2, a.top + std::max(0L, (a.bottom - a.top - h) / 2),
               0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  // Moving to a monitor with a different scale changes the DPI: size again for that monitor.
  win_resize(g_lw, g_lh);
}

void win_show() {
  ShowWindow(g_win, SW_SHOW);
  SetForegroundWindow(g_win);
  fit_widget();
}

void win_minimize() { ShowWindow(g_win, SW_MINIMIZE); }

void win_on_top(bool on) {
  SetWindowPos(g_win, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void win_theme(bool light, bool translucent, unsigned border, unsigned bg) {
  HBRUSH nb = CreateSolidBrush(RGB((bg >> 16) & 255, (bg >> 8) & 255, bg & 255));
  HBRUSH old = g_bg;
  g_bg = nb;
  if (old) DeleteObject(old);
  InvalidateRect(g_win, nullptr, TRUE);
  set_browser_bg(bg);
  set_dwm_int(g_win, 20, light ? 0 : 1);
  COLORREF c = RGB((border >> 16) & 255, (border >> 8) & 255, border & 255);
  DwmSetWindowAttribute(g_win, 34 /*DWMWA_BORDER_COLOR*/, &c, sizeof(c));
  LONG ex = GetWindowLongW(g_win, GWL_EXSTYLE);
  if (translucent) {
    SetWindowLongW(g_win, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    SetLayeredWindowAttributes(g_win, 0, 222, LWA_ALPHA);  // about 87% opaque
  } else if (ex & WS_EX_LAYERED) {
    SetWindowLongW(g_win, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
  }
}

void win_drag_begin() {
  RECT r;
  GetWindowRect(g_win, &r);
  g_drag_start = {r.left, r.top};
  g_drag_dpi = dpi_of(g_win);
}

void win_drag_to(int dx, int dy) {
  SetWindowPos(g_win, nullptr, g_drag_start.x + MulDiv(dx, g_drag_dpi, 96),
               g_drag_start.y + MulDiv(dy, g_drag_dpi, 96), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Hotkeys are read by polling key state ~250 times a second instead of a low-level hook.
// A hook sits in front of EVERY input event on the PC (including the macro's own presses),
// so at high speeds it delays the game's input and causes lag. Polling adds zero delay.
std::atomic<bool> g_poll_run{false};

// Same opt-out per thread, so the hotkey checker and click threads always run at full speed.
void no_throttle_thread() {
  struct TPTS { ULONG Version, ControlMask, StateMask; } t{1, 0x1, 0};
  using SetThreadInfo_t = BOOL(WINAPI *)(HANDLE, int, LPVOID, DWORD);
  if (auto fn = proc<SetThreadInfo_t>(L"kernel32.dll", "SetThreadInformation"))
    fn(GetCurrentThread(), 3 /*ThreadPowerThrottling*/, &t, sizeof(t));
}

void start_hooks(KeyFn on_key, PickFn on_pick, PermFn on_perm) {
  g_key = std::move(on_key);
  g_pick = std::move(on_pick);
  (void)on_perm;  // nothing to ask for on Windows
  g_poll_run = true;
  g_hook_thread = std::thread([] {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    no_throttle_thread();
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    bool prev[256] = {};
    for (int vk = 1; vk < 256; ++vk) prev[vk] = GetAsyncKeyState(vk) & 0x8000;  // ignore keys already held
    while (g_poll_run) {
      for (int vk = 1; vk < 256; ++vk) {
        // skip the generic Shift/Ctrl/Alt codes: the left/right versions (160-165) are reported instead
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_RBUTTON) continue;
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down == prev[vk]) continue;
        prev[vk] = down;
        switch (vk) {
          case VK_LBUTTON:
            if (down) { POINT p; GetCursorPos(&p); g_pick(p.x, p.y); }
            break;
          case VK_MBUTTON: g_key("m:middle", down); break;
          case VK_XBUTTON1: g_key("m:x1", down); break;
          case VK_XBUTTON2: g_key("m:x2", down); break;
          default: g_key("vk:" + std::to_string(vk), down); break;
        }
      }
      if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -40000;  // 4 ms
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, INFINITE);
      } else {
        Sleep(4);
      }
    }
    if (timer) CloseHandle(timer);
  });
}

void stop_hooks() {
  g_poll_run = false;
  if (g_hook_thread.joinable()) {
    WaitForSingleObject(g_hook_thread.native_handle(), 100);
    g_hook_thread.detach();
  }
}

// Last real click, for "Type while clicking" (it must know whether chat was closed by a click)
std::atomic<unsigned long long> g_click_n{0};
std::atomic<long long> g_click_t{0};
static void note_click() {
  g_click_t = (long long)GetTickCount64();
  ++g_click_n;
}

void click(int btn, bool dbl) {
  POINT pt;
  GetCursorPos(&pt);
  if (over_self(pt)) return;  // never click our own window
  note_click();
  Packet &p = g_packets[btn][dbl ? 1 : 0];
  SendInput(p.n, p.in, sizeof(INPUT));
}

void click_at(int x, int y, int btn) {
  POINT pt{x, y};
  if (over_self(pt)) return;
  note_click();
  int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
  int vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
  INPUT in[3]{};
  for (auto &i : in) i.type = INPUT_MOUSE;
  in[0].mi.dx = MulDiv(x - vx, 65535, vw);
  in[0].mi.dy = MulDiv(y - vy, 65535, vh);
  in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  in[1].mi.dwFlags = DOWN[btn];
  in[2].mi.dwFlags = UP[btn];
  SendInput(3, in, sizeof(INPUT));
}

void press_key(const std::string &id) {
  if (GetForegroundWindow() == g_win) return;  // never type into NYC itself
  INPUT in[2]{};
  if (id.rfind("m:", 0) == 0) {
    DWORD d = 0, u = 0, data = 0;
    if (id == "m:middle") { d = MOUSEEVENTF_MIDDLEDOWN; u = MOUSEEVENTF_MIDDLEUP; }
    else { d = MOUSEEVENTF_XDOWN; u = MOUSEEVENTF_XUP; data = id == "m:x1" ? XBUTTON1 : XBUTTON2; }
    for (int i = 0; i < 2; ++i) {
      in[i].type = INPUT_MOUSE;
      in[i].mi.mouseData = data;
      in[i].mi.dwFlags = i ? u : d;
    }
    SendInput(2, in, sizeof(INPUT));
    return;
  }
  if (id.rfind("vk:", 0) != 0) return;
  WORD vk = (WORD)atoi(id.c_str() + 3);
  // Games read hardware scan codes, so send those (with the extended flag for keys like arrows/Insert).
  UINT sc = MapVirtualKeyW(vk, 4 /*MAPVK_VK_TO_VSC_EX*/);
  for (int i = 0; i < 2; ++i) {
    in[i].type = INPUT_KEYBOARD;
    if (sc) {
      in[i].ki.wScan = (WORD)(sc & 0xFF);
      in[i].ki.dwFlags = KEYEVENTF_SCANCODE | (((sc >> 8) == 0xE0 || (sc >> 8) == 0xE1) ? KEYEVENTF_EXTENDEDKEY : 0);
    } else {
      in[i].ki.wVk = vk;
    }
    if (i) in[i].ki.dwFlags |= KEYEVENTF_KEYUP;
  }
  SendInput(2, in, sizeof(INPUT));
}

void engine_thread_begin() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  no_throttle_thread();
  t_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
  std::lock_guard<std::mutex> l(g_timer_m);
  if (++g_timer_users == 1 && pNtSet && g_best_res) {
    ULONG cur;
    pNtSet(g_best_res, TRUE, &cur);
  }
}

void engine_thread_end() {
  if (t_timer) { CloseHandle(t_timer); t_timer = nullptr; }
  std::lock_guard<std::mutex> l(g_timer_m);
  if (g_timer_users > 0 && --g_timer_users == 0 && pNtSet && g_best_res) {
    ULONG cur;
    pNtSet(g_best_res, FALSE, &cur);
  }
}

bool wait_until(Clock::time_point t, const std::atomic<bool> &running) {
  using namespace std::chrono;
  for (;;) {
    auto rem = t - Clock::now();
    if (rem <= Clock::duration::zero()) return true;
    if (!running) return false;
    auto us = duration_cast<microseconds>(rem).count();
    if (us > 600) {
      // sleep most of the way (max 10 ms per step so stopping is instant)
      sleep_for_100ns(std::min<long long>(us - 300, 10000) * 10);
    } else if (us > 40) {
      SwitchToThread();  // give the CPU to the game instead of hogging it
    } else {
      _mm_pause();  // only the last few microseconds are spun
    }
  }
}

std::string key_name(const std::string &id) {
  if (id.empty()) return "Not set";
  if (id == "m:middle") return "Mouse 3";
  if (id == "m:x1") return "Mouse 4";
  if (id == "m:x2") return "Mouse 5";
  if (id.rfind("vk:", 0) != 0) return id;
  int vk = atoi(id.c_str() + 3);
  if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x6F);
  if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, (char)vk);
  if (vk >= 0x60 && vk <= 0x69) return "Num " + std::to_string(vk - 0x60);
  switch (vk) {
    case 8: return "Backspace"; case 9: return "Tab"; case 13: return "Enter"; case 16: return "Shift";
    case 17: return "Ctrl"; case 18: return "Alt"; case 19: return "Pause"; case 20: return "Caps Lock";
    case 27: return "Esc"; case 32: return "Space"; case 33: return "Page Up"; case 34: return "Page Down";
    case 35: return "End"; case 36: return "Home"; case 37: return "Left"; case 38: return "Up";
    case 39: return "Right"; case 40: return "Down"; case 44: return "Print Screen"; case 45: return "Insert";
    case 46: return "Delete"; case 91: return "Win"; case 92: return "Right Win"; case 93: return "Menu";
    case 106: return "Num *"; case 107: return "Num +"; case 109: return "Num -"; case 110: return "Num .";
    case 111: return "Num /"; case 144: return "Num Lock"; case 145: return "Scroll Lock";
    case 160: return "Left Shift"; case 161: return "Right Shift"; case 162: return "Left Ctrl";
    case 163: return "Right Ctrl"; case 164: return "Left Alt"; case 165: return "Right Alt";
    case 186: return ";"; case 187: return "="; case 188: return ","; case 189: return "-"; case 190: return ".";
    case 191: return "/"; case 192: return "`"; case 219: return "["; case 220: return "\\"; case 221: return "]";
    case 222: return "'";
  }
  wchar_t buf[64];
  UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
  if (sc && GetKeyNameTextW((LONG)(sc << 16), buf, 64) > 0) {
    char out[128];
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out), nullptr, nullptr);
    if (n > 1) return std::string(out);
  }
  return "Key " + std::to_string(vk);
}

std::string default_hotkey() { return "vk:117"; }  // F6
std::string default_killkey() { return "vk:45"; }  // Insert
std::string default_keyhot() { return "vk:118"; }  // F7
bool is_escape(const std::string &id) { return id == "vk:27"; }
bool is_clear_key(const std::string &id) { return id == "vk:8" || id == "vk:46"; }

std::filesystem::path config_dir() {
  PWSTR p = nullptr;
  std::filesystem::path base;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) base = p;
  if (p) CoTaskMemFree(p);
  if (base.empty()) base = std::filesystem::temp_directory_path();
  return base / L"BeagleAutoclicker";
}

void show_error(const std::string &msg) {
  int n = MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n : 1, L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, &w[0], n);
  MessageBoxW(nullptr, w.c_str(), L"NYC", MB_OK | MB_ICONERROR);
}

std::filesystem::path choose_icon_file() {
  wchar_t buf[MAX_PATH * 4] = L"";
  OPENFILENAMEW of{};
  of.lStructSize = sizeof(of);
  of.hwndOwner = g_win;
  of.lpstrFilter = L"Images (PNG, JPG, ICO, BMP, GIF)\0*.png;*.jpg;*.jpeg;*.ico;*.bmp;*.gif;*.webp\0All files\0*.*\0";
  of.lpstrFile = buf;
  of.nMaxFile = MAX_PATH * 4;
  of.lpstrTitle = L"Choose an icon image";
  of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (!GetOpenFileNameW(&of)) return {};
  return std::filesystem::path(buf);
}

std::filesystem::path choose_export_file() {
  wchar_t buf[MAX_PATH * 4] = L"NYC settings.json";
  OPENFILENAMEW of{};
  of.lStructSize = sizeof(of);
  of.hwndOwner = g_win;
  of.lpstrFilter = L"NYC settings (*.json)\0*.json\0";
  of.lpstrDefExt = L"json";
  of.lpstrFile = buf;
  of.nMaxFile = MAX_PATH * 4;
  of.lpstrTitle = L"Export settings";
  of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (!GetSaveFileNameW(&of)) return {};
  return std::filesystem::path(buf);
}

std::filesystem::path choose_import_file() {
  wchar_t buf[MAX_PATH * 4] = L"";
  OPENFILENAMEW of{};
  of.lStructSize = sizeof(of);
  of.hwndOwner = g_win;
  of.lpstrFilter = L"NYC settings (*.json)\0*.json\0All files\0*.*\0";
  of.lpstrFile = buf;
  of.nMaxFile = MAX_PATH * 4;
  of.lpstrTitle = L"Import settings";
  of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (!GetOpenFileNameW(&of)) return {};
  return std::filesystem::path(buf);
}

bool apply_icon(const std::filesystem::path &image) {
  int dpi = dpi_of(g_win);
  int big = MulDiv(GetSystemMetrics(SM_CXICON), dpi, 96);
  int small = MulDiv(GetSystemMetrics(SM_CXSMICON), dpi, 96);
  HICON b = nullptr, s = nullptr;
  bool custom = !image.empty();
  if (custom) {
    b = icon_from_image(image, big);
    s = icon_from_image(image, small);
    if (!b || !s) {
      if (b) DestroyIcon(b);
      if (s) DestroyIcon(s);
      return false;
    }
  } else {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    b = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, big, big, 0);
    s = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, small, small, 0);
  }
  // If NYC is pinned to the taskbar, Windows shows the pinned shortcut's icon and ignores the
  // window icon. Giving the window its own app ID while a custom icon is set makes the taskbar
  // treat it as its own button, which uses the window icon below.
  {
    static const PROPERTYKEY AUMID = {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};
    IPropertyStore *ps = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(g_win, IID_PPV_ARGS(&ps))) && ps) {
      PROPVARIANT pv;
      PropVariantInit(&pv);
      if (custom) {
        static wchar_t id[] = L"BeaglewareTM.NYC.CustomIcon";
        pv.vt = VT_LPWSTR;
        pv.pwszVal = id;
      }
      ps->SetValue(AUMID, pv);
      ps->Commit();
      ps->Release();
    }
  }
  SendMessageW(g_win, WM_SETICON, ICON_BIG, (LPARAM)b);
  SendMessageW(g_win, WM_SETICON, ICON_SMALL, (LPARAM)s);
  if (g_icon_big) DestroyIcon(g_icon_big);
  if (g_icon_small) DestroyIcon(g_icon_small);
  g_icon_big = b;
  g_icon_small = s;
  return true;
}

void quit_now() {
  if (g_win) ShowWindow(g_win, SW_HIDE);  // vanish instantly
  // Settings are already saved. Terminate skips the slow browser teardown entirely.
  TerminateProcess(GetCurrentProcess(), 0);
  for (;;) {}
}

}  // namespace plat

// ---------------------------------------------------------------- auto update
namespace plat {
namespace {
std::wstring exe_path() {
  wchar_t b[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH * 4);
  return std::wstring(b, n);
}
std::wstring new_path() { return exe_path() + L".new"; }
std::wstring old_path() { return exe_path() + L".old"; }

// GET https://host/path, following redirects. Returns false on any error or non-200.
bool https_get(const std::wstring &host, const std::wstring &path, std::string &out, size_t cap) {
  bool ok = false;
  HINTERNET s = WinHttpOpen(L"NYC-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
  if (!s) s = WinHttpOpen(L"NYC-Updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
  if (!s) return false;
  WinHttpSetTimeouts(s, 8000, 8000, 15000, 60000);
  HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE) : nullptr;
  if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json, application/octet-stream\r\n", (DWORD)-1, nullptr, 0, 0, 0) &&
      WinHttpReceiveResponse(r, nullptr)) {
    DWORD code = 0, sz = sizeof(code);
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz, nullptr);
    if (code == 200) {
      ok = true;
      char buf[16384];
      DWORD got = 0;
      while (WinHttpReadData(r, buf, sizeof(buf), &got) && got) {
        out.append(buf, got);
        if (out.size() > cap) { ok = false; break; }
      }
    }
  }
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return ok;
}

std::vector<int> ver(std::string v) {
  std::vector<int> o;
  int cur = -1;
  for (char ch : v) {
    if (ch >= '0' && ch <= '9') cur = (cur < 0 ? 0 : cur) * 10 + (ch - '0');
    else if (cur >= 0) { o.push_back(cur); cur = -1; }
  }
  if (cur >= 0) o.push_back(cur);
  while (o.size() < 3) o.push_back(0);
  return o;
}
}  // namespace

void update_cleanup() {
  DeleteFileW(old_path().c_str());
  DeleteFileW(new_path().c_str());
}

bool update_fetch(const char *repo, const char *current) {
  try {
    std::string body;
    std::string p = std::string("/repos/") + repo + "/releases/latest";
    if (!https_get(L"api.github.com", std::wstring(p.begin(), p.end()), body, 4 << 20)) return false;
    nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("tag_name") || !j["tag_name"].is_string()) return false;
    if (!(ver(j["tag_name"].get<std::string>()) > ver(current))) return false;
    std::string url;
    for (auto &a : j.value("assets", nlohmann::json::array()))
      if (a.is_object() && a.value("name", "") == "NYC.exe") url = a.value("browser_download_url", "");
    const std::string pre = "https://";
    if (url.compare(0, pre.size(), pre) != 0) return false;
    url = url.substr(pre.size());
    size_t sl = url.find('/');
    if (sl == std::string::npos) return false;
    std::string host = url.substr(0, sl), path = url.substr(sl);
    std::string exe;
    if (!https_get(std::wstring(host.begin(), host.end()), std::wstring(path.begin(), path.end()), exe, 64 << 20)) return false;
    if (exe.size() < 200000 || exe[0] != 'M' || exe[1] != 'Z') return false;  // must look like a real exe
    std::wstring np = new_path();
    HANDLE f = CreateFileW(np.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(f, exe.data(), (DWORD)exe.size(), &w, nullptr) && w == exe.size();
    CloseHandle(f);
    if (!ok) DeleteFileW(np.c_str());
    return ok;
  } catch (...) { return false; }
}

void update_install_and_restart() {
  std::wstring cur = exe_path(), old = old_path(), np = new_path();
  DeleteFileW(old.c_str());
  // A running exe can be renamed but not overwritten: move it aside, put the new one in its place.
  if (MoveFileExW(cur.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    if (!MoveFileExW(np.c_str(), cur.c_str(), MOVEFILE_REPLACE_EXISTING))
      MoveFileExW(old.c_str(), cur.c_str(), MOVEFILE_REPLACE_EXISTING);  // undo, keep working copy
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + cur + L"\"";
  if (CreateProcessW(cur.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  quit_now();
}
}  // namespace plat

// ---------------------------------------------------------------- type while clicking
namespace plat {
namespace {
struct ChatKey { wchar_t ch; WORD vk; };  // ch for text, vk for Enter / Backspace
std::mutex g_chat_m;
std::vector<ChatKey> g_chat_q;
std::vector<WORD> g_chat_keep;
bool g_chat_swallowed[256];
std::atomic<bool> g_chat_on{false};
std::thread g_chat_th;
DWORD g_chat_tid = 0;

LRESULT CALLBACK chat_hook(int code, WPARAM wp, LPARAM lp) {
  if (code == HC_ACTION && g_chat_on) {
    auto *k = reinterpret_cast<KBDLLHOOKSTRUCT *>(lp);
    WORD vk = (WORD)k->vkCode;
    bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
    bool up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
    if (!(k->flags & LLKHF_INJECTED) && vk < 256) {
      if (up && g_chat_swallowed[vk]) { g_chat_swallowed[vk] = false; return 1; }
      if (down && GetForegroundWindow() != g_win) {
        bool keep = vk == VK_OEM_2 || vk == VK_ESCAPE;  // "/" and Esc go straight to Roblox
        {
          std::lock_guard<std::mutex> l(g_chat_m);
          for (WORD w : g_chat_keep) keep = keep || w == vk;
        }
        bool mod = (GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_LWIN) |
                    GetAsyncKeyState(VK_RWIN)) & 0x8000;
        if (!keep && !mod) {
          ChatKey ck{0, 0};
          if (vk == VK_RETURN || vk == VK_BACK) ck.vk = vk;
          else {
            BYTE st[256]{};
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) st[VK_SHIFT] = 0x80;
            if (GetKeyState(VK_CAPITAL) & 1) st[VK_CAPITAL] = 1;
            wchar_t buf[4];
            int n = ToUnicodeEx(vk, k->scanCode, st, buf, 4, 4 /*don't change keyboard state*/, GetKeyboardLayout(0));
            if (n == 1 && buf[0] >= 32) ck.ch = buf[0];
          }
          if (ck.ch || ck.vk) {
            { std::lock_guard<std::mutex> l(g_chat_m); g_chat_q.push_back(ck); }
            g_chat_swallowed[vk] = true;
            return 1;  // keep it away from the game; it's typed into chat between clicks
          }
        }
      }
    }
  }
  return CallNextHookEx(nullptr, code, wp, lp);
}

void send_vk(WORD vk) {
  INPUT in[2]{};
  UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
  for (int i = 0; i < 2; ++i) {
    in[i].type = INPUT_KEYBOARD;
    in[i].ki.wVk = vk;
    in[i].ki.wScan = (WORD)sc;
    if (i) in[i].ki.dwFlags = KEYEVENTF_KEYUP;
  }
  SendInput(2, in, sizeof(INPUT));
}
void send_char(wchar_t ch) {
  INPUT in[2]{};
  for (int i = 0; i < 2; ++i) {
    in[i].type = INPUT_KEYBOARD;
    in[i].ki.wScan = ch;
    in[i].ki.dwFlags = KEYEVENTF_UNICODE | (i ? KEYEVENTF_KEYUP : 0);
  }
  SendInput(2, in, sizeof(INPUT));
}
WORD vk_of(const std::string &id) { return id.rfind("vk:", 0) == 0 ? (WORD)atoi(id.c_str() + 3) : 0; }
}  // namespace

void chat_relay(bool on, const std::vector<std::string> &keep) {
  {
    std::lock_guard<std::mutex> l(g_chat_m);
    g_chat_keep.clear();
    for (auto &k : keep) if (WORD v = vk_of(k)) g_chat_keep.push_back(v);
    if (!on) g_chat_q.clear();
  }
  if (on == g_chat_on) return;
  if (on) {
    g_chat_on = true;
    std::atomic<DWORD> tid{0};
    g_chat_th = std::thread([&tid] {
      MSG m;
      PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);  // create the message queue
      HHOOK h = SetWindowsHookExW(WH_KEYBOARD_LL, chat_hook, GetModuleHandleW(nullptr), 0);
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);  // hook must answer fast
      tid = GetCurrentThreadId();
      while (GetMessageW(&m, nullptr, 0, 0) > 0) {}
      if (h) UnhookWindowsHookEx(h);
    });
    while (!tid) std::this_thread::yield();
    g_chat_tid = tid;
  } else {
    g_chat_on = false;
    PostThreadMessageW(g_chat_tid, WM_QUIT, 0, 0);
    if (g_chat_th.joinable()) g_chat_th.join();
    for (bool &b : g_chat_swallowed) b = false;
  }
}

bool chat_flush() {
  std::vector<ChatKey> q;
  { std::lock_guard<std::mutex> l(g_chat_m); q.swap(g_chat_q); }
  if (q.empty()) return false;
  if (GetForegroundWindow() == g_win) return true;
  static unsigned long long last_n = ~0ull;
  if (g_click_n != last_n) {
    // A click happened since we last typed, so chat is (or is about to be) closed. Wait until
    // Roblox has actually closed it, otherwise our "/" lands in the chat box as text.
    long long since = (long long)GetTickCount64() - g_click_t;
    if (since < 45) Sleep((DWORD)(45 - since));
    send_vk(VK_OEM_2);  // "/" opens Roblox chat (cursor goes back into the box, text kept)
    Sleep(35);          // Roblox needs a frame or two before the chat box takes letters
  }
  // else: no click since last time, so chat is still open; type straight in, no "/"
  for (auto &k : q) {
    if (k.vk) send_vk(k.vk);
    else send_char(k.ch);
  }
  Sleep(15);  // let Roblox take the letters before the next click closes the box
  last_n = g_click_n;
  return true;
}
}  // namespace plat
