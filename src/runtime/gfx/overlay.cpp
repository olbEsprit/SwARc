#include "runtime/gfx/overlay.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>

namespace recomp::gfx::overlay {
namespace {

// 5x7 glyphs, a row per byte, bit 4 the leftmost column; lower case is drawn as upper case.
const char kChars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,:-+/%()<>='!?*_";
const uint8_t kGlyphs[][7] = {
    {0, 0, 0, 0, 0, 0, 0},
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},
    {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
    {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},
    {7, 2, 2, 2, 2, 18, 12},      {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 17, 25, 21, 19, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
    {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
    {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},       {17, 17, 17, 17, 17, 17, 14},
    {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 17, 10, 4, 4, 4},    {31, 1, 2, 4, 8, 16, 31},
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},      {14, 17, 1, 2, 4, 8, 31},
    {31, 2, 4, 2, 1, 17, 14},     {2, 6, 10, 18, 31, 2, 2},     {31, 16, 30, 1, 1, 17, 14},
    {6, 8, 16, 30, 17, 17, 14},   {31, 1, 2, 4, 8, 8, 8},       {14, 17, 17, 14, 17, 17, 14},
    {14, 17, 17, 15, 1, 2, 12},
    {0, 0, 0, 0, 0, 12, 12},      {0, 0, 0, 0, 12, 4, 8},       {0, 12, 12, 0, 12, 12, 0},
    {0, 0, 0, 31, 0, 0, 0},       {0, 4, 4, 31, 4, 4, 0},       {0, 1, 2, 4, 8, 16, 0},
    {24, 25, 2, 4, 8, 19, 3},     {2, 4, 8, 8, 8, 4, 2},        {8, 4, 2, 2, 2, 4, 8},
    {2, 4, 8, 16, 8, 4, 2},       {8, 4, 2, 1, 2, 4, 8},        {0, 0, 31, 0, 31, 0, 0},
    {4, 4, 8, 0, 0, 0, 0},        {4, 4, 4, 4, 4, 0, 4},        {14, 17, 1, 2, 4, 0, 4},
    {0, 4, 21, 14, 21, 4, 0},     {0, 0, 0, 0, 0, 0, 31}};
static_assert(sizeof kGlyphs / sizeof kGlyphs[0] == sizeof kChars - 1);

constexpr int kCellW = 6, kCellH = 10;
constexpr uint32_t kBg = 0xFF181410, kText = 0xFFE0E0E0, kDim = 0xFF909090, kAccent = 0xFF1E8CFF,
                   kSelBg = 0xFF1E6ED8, kSelText = 0xFF101010;

const uint8_t* Glyph(char c) {
  const char* p = std::strchr(kChars, std::toupper(static_cast<unsigned char>(c)));
  return kGlyphs[p && c ? p - kChars : 0];
}

struct Canvas {
  Layer& l;
  void Fill(int x, int y, int w, int h, uint32_t c) {
    for (int j = std::max(0, y); j < std::min(l.h, y + h); j++)
      for (int i = std::max(0, x); i < std::min(l.w, x + w); i++) l.px[size_t(j) * l.w + i] = c;
  }
  void Text(int x, int y, const std::string& s, uint32_t c) {
    for (size_t n = 0; n < s.size(); n++) {
      const uint8_t* g = Glyph(s[n]);
      for (int j = 0; j < 7; j++)
        for (int i = 0; i < 5; i++)
          if (g[j] & (16 >> i)) Fill(x + int(n) * kCellW + i, y + j, 1, 1, c);
    }
  }
};

std::mutex g_mu;
std::vector<Item> g_items;
std::map<std::string, std::string> g_saved;
std::string g_ini, g_title = "SETTINGS";
bool g_open = false, g_suppress = false;
uint32_t g_held = 0;
int g_cursor = 0;
uint64_t g_menu_version = 1;

int g_tele = 0;
std::atomic<int> g_aniso{0};
std::atomic<int> g_deadzone{12};
struct Stats {
  std::vector<double> dts;
  double present_ms = 0, compile_ms = 0, compile_ms_total = 0;
  int compiles = 0, compiles_total = 0, w = 0, h = 0;
  uint64_t last = 0, start = 0;
} g_stats;
Layer g_tele_layer;

void Save() {
  if (g_ini.empty()) return;
  for (const Item& it : g_items) g_saved[it.key] = it.values[size_t(it.value)];
  std::ofstream f(g_ini);
  for (const auto& [k, v] : g_saved) f << k << '=' << v << '\n';
}

int SavedIndex(const Item& it) {
  auto s = g_saved.find(it.key);
  if (s == g_saved.end()) return it.value;
  auto v = std::find(it.values.begin(), it.values.end(), s->second);
  return v == it.values.end() ? it.value : int(v - it.values.begin());
}

void EnsureBuiltins() {
  if (!g_items.empty()) return;
  g_items.push_back({"telemetry", "Telemetry", {"Off", "FPS", "Detail"}, [](int v) { g_tele = v; }, 0});
  g_items.push_back({"deadzone", "Stick dead zone", {"8%", "12%", "16%", "20%", "25%"},
                     [](int v) {
                       static const int kPercent[] = {8, 12, 16, 20, 25};
                       g_deadzone = kPercent[v];
                     },
                     1});
  g_items.push_back({"anisotropy", "Anisotropic filtering", {"Game", "2x", "4x", "8x", "16x"},
                     [](int v) { g_aniso = v ? 1 << v : 0; }, 0});
}

// Changes item `i` under g_mu; returns its apply call to make after unlocking.
std::function<void()> SetLocked(size_t i, int v) {
  Item& it = g_items[i];
  v = (v + int(it.values.size())) % int(it.values.size());
  if (v == it.value) return {};
  it.value = v;
  g_menu_version++;
  Save();
  return it.apply ? std::function<void()>([f = it.apply, v] { f(v); }) : std::function<void()>();
}

}  // namespace

void Add(Item item) {
  std::function<void()> apply;
  {
    std::lock_guard<std::mutex> l(g_mu);
    EnsureBuiltins();
    item.value = SavedIndex(item);
    if (item.apply) apply = [f = item.apply, v = item.value] { f(v); };
    g_items.push_back(std::move(item));
    g_menu_version++;
  }
  if (apply) apply();
}

int Value(const std::string& key) {
  std::lock_guard<std::mutex> l(g_mu);
  EnsureBuiltins();
  for (const Item& it : g_items)
    if (it.key == key) return it.value;
  return 0;
}

void SetIni(const std::string& path) {
  std::vector<std::function<void()>> applies;
  {
    std::lock_guard<std::mutex> l(g_mu);
    EnsureBuiltins();
    g_ini = path;
    std::ifstream f(path);
    for (std::string line; std::getline(f, line);) {
      const size_t eq = line.find('=');
      if (eq != std::string::npos) g_saved[line.substr(0, eq)] = line.substr(eq + 1);
    }
    for (Item& it : g_items) {
      const int v = SavedIndex(it);
      if (v != it.value && it.apply) applies.push_back([f = it.apply, v] { f(v); });
      it.value = v;
    }
    g_menu_version++;
  }
  for (auto& a : applies) a();
}

void SetTitle(const std::string& title) {
  std::lock_guard<std::mutex> l(g_mu);
  g_title = title;
  g_menu_version++;
}

void Press(Input in) {
  std::function<void()> apply;
  {
    std::lock_guard<std::mutex> l(g_mu);
    EnsureBuiltins();
    if (in == Input::Toggle) {
      g_open = !g_open;
      g_suppress = true;
    } else if (g_open) {
      const int n = int(g_items.size());
      switch (in) {
        case Input::Up: g_cursor = (g_cursor + n - 1) % n; break;
        case Input::Down: g_cursor = (g_cursor + 1) % n; break;
        case Input::Left: apply = SetLocked(size_t(g_cursor), g_items[size_t(g_cursor)].value - 1); break;
        case Input::Right:
        case Input::Accept: apply = SetLocked(size_t(g_cursor), g_items[size_t(g_cursor)].value + 1); break;
        case Input::Back: g_open = false; break;
        default: break;
      }
    }
    g_menu_version++;
  }
  if (apply) apply();
}

bool IsOpen() {
  std::lock_guard<std::mutex> l(g_mu);
  return g_open;
}

void NotePadButton(int button, bool down) {
  bool chord = false;
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (button < 0 || button >= 32) return;
    if (down) g_held |= 1u << button;
    else g_held &= ~(1u << button);
    if (!g_held && !g_open) g_suppress = false;
    const uint32_t sticks = (1u << SDL_GAMEPAD_BUTTON_LEFT_STICK) | (1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    chord = down && (g_held & sticks) == sticks &&
            (button == SDL_GAMEPAD_BUTTON_LEFT_STICK || button == SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    if (!chord && (!g_open || !down)) return;
  }
  if (chord) return Press(Input::Toggle);
  switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: Press(Input::Up); break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: Press(Input::Down); break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: Press(Input::Left); break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: Press(Input::Right); break;
    case SDL_GAMEPAD_BUTTON_SOUTH: Press(Input::Accept); break;
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_START: Press(Input::Back); break;
    default: break;
  }
}

bool CapturesPad() {
  std::lock_guard<std::mutex> l(g_mu);
  return g_open || g_suppress;
}

int Anisotropy() { return g_aniso; }
int StickDeadZone() { return g_deadzone; }

int TelemetryMode() {
  std::lock_guard<std::mutex> l(g_mu);
  return g_tele;
}

void SetTelemetry(int mode) {
  std::function<void()> apply;
  {
    std::lock_guard<std::mutex> l(g_mu);
    EnsureBuiltins();
    apply = SetLocked(0, mode);
  }
  if (apply) apply();
}

void NoteShaderCompile(double ms) {
  std::lock_guard<std::mutex> l(g_mu);
  g_stats.compiles++;
  g_stats.compiles_total++;
  g_stats.compile_ms += ms;
  g_stats.compile_ms_total += ms;
}

void NoteFrame(double present_ms, int w, int h) {
  std::lock_guard<std::mutex> l(g_mu);
  Stats& s = g_stats;
  const uint64_t now = SDL_GetTicksNS();
  if (s.last) s.dts.push_back(double(now - s.last) / 1e6);
  s.last = now;
  s.present_ms += present_ms;
  s.w = w;
  s.h = h;
  if (!s.start) s.start = now;
  if (now - s.start < 1000000000ull || s.dts.empty()) return;
  std::sort(s.dts.begin(), s.dts.end());
  auto q = [&](double p) { return s.dts[size_t(p * double(s.dts.size() - 1) + 0.5)]; };
  const double n = double(s.dts.size());
  char lines[4][48];
  std::snprintf(lines[0], sizeof lines[0], "%5.1f FPS  %dX%d", n * 1e9 / double(now - s.start), w, h);
  std::snprintf(lines[1], sizeof lines[1], "FRAME P50 %4.1f P99 %4.1f MAX %5.1f", q(0.5), q(0.99), s.dts.back());
  std::snprintf(lines[2], sizeof lines[2], "PRESENT %4.1f MS", s.present_ms / n);
  std::snprintf(lines[3], sizeof lines[3], "SHADERS %d %5.1f MS  TOTAL %d %.0f MS", s.compiles, s.compile_ms,
                s.compiles_total, s.compile_ms_total);
  const int rows = g_tele == 2 ? 4 : 1;
  size_t cols = 0;
  for (int r = 0; r < rows; r++) cols = std::max(cols, std::strlen(lines[r]));
  if (g_tele == 1) cols = 9;
  Layer& L = g_tele_layer;
  L.w = int(cols) * kCellW + 4;
  L.h = rows * kCellH;
  L.px.assign(size_t(L.w) * L.h, 0xFF000000u);
  Canvas cv{L};
  for (int r = 0; r < rows; r++) cv.Text(2, 2 + r * kCellH, std::string(lines[r], g_tele == 1 ? 9 : std::strlen(lines[r])), 0xFF60FF60u);
  L.version++;
  s.dts.clear();
  s.present_ms = s.compile_ms = 0;
  s.compiles = 0;
  s.start = now;
}

bool Telemetry(Layer* out) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_tele || !g_tele_layer.version) return false;
  if (out->version != g_tele_layer.version) *out = g_tele_layer;
  return true;
}

bool Menu(Layer* out) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_open) return false;
  if (out->version == g_menu_version) return true;
  constexpr int kCols = 40;
  const int rows = int(g_items.size()) + 4;
  out->w = kCols * kCellW + 8;
  out->h = rows * kCellH + 6;
  out->px.assign(size_t(out->w) * out->h, kBg);
  Canvas cv{*out};
  cv.Text(4 + (kCols - int(g_title.size())) / 2 * kCellW, 5, g_title, kAccent);
  for (size_t i = 0; i < g_items.size(); i++) {
    const Item& it = g_items[i];
    const int y = 5 + int(i + 2) * kCellH;
    const bool sel = int(i) == g_cursor;
    std::string v = it.values[size_t(it.value)];
    if (sel) v = "< " + v + " >";
    if (sel) cv.Fill(2, y - 2, out->w - 4, kCellH - 1, kSelBg);
    const uint32_t c = sel ? kSelText : kText;
    cv.Text(4 + kCellW, y, it.label, c);
    cv.Text(4 + (kCols - 1 - int(v.size())) * kCellW, y, v, c);
  }
  cv.Text(4 + kCellW, 5 + (rows - 1) * kCellH, "A CHANGE   B CLOSE   L3+R3 / F4", kDim);
  out->version = g_menu_version;
  return true;
}

}  // namespace recomp::gfx::overlay
