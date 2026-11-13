#pragma once
// The runtime's menu over the game (pad: L3+R3; keyboard: F4) and its
// telemetry (F3 cycles it, or the menu's first item). A port adds its own
// settings as items; their values are kept in the file SetIni names.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace recomp::gfx::overlay {

struct Item {
  std::string key;  // in the ini
  std::string label;
  std::vector<std::string> values;
  std::function<void(int)> apply;  // with the value's index, on load and on every change
  int value = 0;
};

// Adds an item below the existing ones, with its saved value, and applies it.
void Add(Item item);
int Value(const std::string& key);
// Reads the saved values from `path` and saves changes there.
void SetIni(const std::string& path);
void SetTitle(const std::string& title);

enum class Input { Up, Down, Left, Right, Accept, Back, Toggle };
void Press(Input in);
bool IsOpen();
// From the event loop: SDL_GamepadButton presses. L3+R3 opens and closes the
// menu, which takes the D-pad, A (accept), B and Start (close).
void NotePadButton(int button, bool down);
// The pads' state belongs to the menu: it is open, or a button pressed for it
// is still held.
bool CapturesPad();

// The menu's minimum anisotropy for mipmapped linear sampling; 0: the game's.
int Anisotropy();

// The pads' sticks' dead zone, in percent of the throw.
int StickDeadZone();

// 0 off, 1 FPS, 2 frame times, presentation and shader compiles.
int TelemetryMode();
void SetTelemetry(int mode);
void NoteShaderCompile(double ms);
// From Present: the time it took and the game's back buffer size.
void NoteFrame(double present_ms, int w, int h);

// RGBA8 pixels to draw over the window; `version` changes with them.
struct Layer {
  std::vector<uint32_t> px;
  int w = 0, h = 0;
  uint64_t version = 0;
};
// False when the layer is not shown; otherwise `out` is updated if its
// version is older.
bool Menu(Layer* out);
bool Telemetry(Layer* out);

}  // namespace recomp::gfx::overlay
