// Finds a Booga Booga Bloodfruit in an inventory screenshot: a dark red cube sitting inside a
// light grey inventory slot. Red grass, the red "bag" bar and other red things are ignored
// because they aren't surrounded by slot grey.
#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace food {

// pixels: 32-bit BGRA rows, top to bottom. Returns true and the centre (in image pixels) if found.
inline bool find_bloodfruit(const uint8_t *px, int w, int h, int stride, int &ox, int &oy) {
  if (!px || w < 50 || h < 50) return false;
  auto at = [&](int x, int y, int &r, int &g, int &b) {
    const uint8_t *p = px + (size_t)y * stride + (size_t)x * 4;
    b = p[0]; g = p[1]; r = p[2];
  };
  auto maroon = [](int r, int g, int b) {
    return r >= 45 && r <= 120 && g <= 55 && b <= 55 && r - g >= 25 && (g > b ? g - b : b - g) <= 15;
  };
  auto grey = [](int r, int g, int b) {
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    return mn >= 195 && mx - mn <= 8;
  };
  // Slots get bigger on bigger screens; scale the search to the screen height (tuned on 1440p).
  int ring = h * 40 / 1440;
  if (ring < 16) ring = 16;
  int cell = ring * 2;
  struct Acc { long sx = 0, sy = 0; int n = 0; };
  std::unordered_map<long long, Acc> cells;
  for (int y = 0; y < h; y += 2)
    for (int x = 0; x < w; x += 2) {
      int r, g, b;
      at(x, y, r, g, b);
      if (!maroon(r, g, b)) continue;
      Acc &a = cells[(long long)(y / cell) * 100000 + x / cell];
      a.sx += x; a.sy += y; a.n++;
    }
  int min_hits = (ring * ring) / 200 + 4;  // a real cube, not a few stray pixels
  int best_n = 0;
  for (auto &kv : cells) {
    const Acc &a = kv.second;
    if (a.n < min_hits || a.n <= best_n) continue;
    int cx = (int)(a.sx / a.n), cy = (int)(a.sy / a.n);
    const int dirs[7][2] = {{-4, 0}, {4, 0}, {0, -4}, {-3, -3}, {3, -3}, {-4, -2}, {4, -2}};
    int ok = 0;
    for (auto &d : dirs) {
      int x = cx + d[0] * ring / 4, y = cy + d[1] * ring / 4;
      if (x < 0 || y < 0 || x >= w || y >= h) continue;
      int r, g, b;
      at(x, y, r, g, b);
      if (grey(r, g, b)) ok++;
    }
    if (ok >= 5) { best_n = a.n; ox = cx; oy = cy; }
  }
  return best_n > 0;
}

}  // namespace food
