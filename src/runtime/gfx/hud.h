#pragma once
// Frame-time overlay: a small CPU-drawn label the backend blits over the
// window after the game's frame. <ID>_HUD=0 turns it off, F3 toggles it.

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace recomp::gfx {

inline std::atomic<bool> g_hud_on{true};

class Hud {
 public:
  static constexpr int kCols = 29, kCell = 6, kW = kCols * kCell + 4, kH = 2 * 8 + 4;

  void NoteFrame() {
    const Uint64 now = SDL_GetTicksNS();
    if (last_) dts_.push_back(double(now - last_) / 1e6);
    last_ = now;
    if (!start_) start_ = now;
    if (now - start_ < 1000000000ull || dts_.empty()) return;
    std::sort(dts_.begin(), dts_.end());
    auto q = [&](double p) { return dts_[size_t(p * double(dts_.size() - 1) + 0.5)]; };
    char l1[kCols + 1], l2[kCols + 1];
    std::snprintf(l1, sizeof l1, "%5.1f FPS", double(dts_.size()) * 1e9 / double(now - start_));
    std::snprintf(l2, sizeof l2, "P50 %5.1f P99 %5.1f MAX %5.1f", q(0.5), q(0.99), dts_.back());
    Draw(l1, l2);
    dts_.clear();
    start_ = now;
    dirty_ = true;
  }

  bool dirty() const { return dirty_; }
  const uint32_t* pixels() { dirty_ = false; return px_.data(); }

 private:
  static const uint8_t* Glyph(char c) {
    static const uint8_t g[][7] = {
        {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},     {14, 17, 1, 2, 4, 8, 31},
        {30, 1, 1, 14, 1, 1, 30},     {2, 6, 10, 18, 31, 2, 2},    {31, 16, 30, 1, 1, 17, 14},
        {6, 8, 16, 30, 17, 17, 14},   {31, 1, 2, 4, 8, 8, 8},      {14, 17, 17, 14, 17, 17, 14},
        {14, 17, 17, 15, 1, 2, 12},   {0, 0, 0, 0, 0, 12, 12},     {31, 16, 16, 30, 16, 16, 16},
        {30, 17, 17, 30, 16, 16, 16}, {15, 16, 16, 14, 1, 1, 30},  {17, 27, 21, 21, 17, 17, 17},
        {14, 17, 17, 31, 17, 17, 17}, {17, 17, 10, 4, 10, 17, 17}, {0, 0, 0, 0, 0, 0, 0}};
    if (c >= '0' && c <= '9') return g[c - '0'];
    switch (c) {
      case '.': return g[10];
      case 'F': return g[11];
      case 'P': return g[12];
      case 'S': return g[13];
      case 'M': return g[14];
      case 'A': return g[15];
      case 'X': return g[16];
      default: return g[17];
    }
  }

  void Draw(const char* l1, const char* l2) {
    px_.assign(size_t(kW) * kH, 0xFF000000u);
    const char* lines[2] = {l1, l2};
    for (int ln = 0; ln < 2; ln++)
      for (int i = 0; lines[ln][i]; i++) {
        const uint8_t* g = Glyph(lines[ln][i]);
        for (int y = 0; y < 7; y++)
          for (int x = 0; x < 5; x++)
            if (g[y] & (16 >> x))
              px_[size_t(2 + ln * 8 + y) * kW + size_t(2 + i * kCell + x)] = 0xFF60FF60u;
      }
  }

  std::vector<double> dts_;
  std::vector<uint32_t> px_;
  Uint64 last_ = 0, start_ = 0;
  bool dirty_ = false;
};

}  // namespace recomp::gfx
