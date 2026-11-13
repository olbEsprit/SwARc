#pragma once
// The XInput pads (xinput.cpp) as other APIs list them: DirectInput's game
// controllers (dinput8.cpp) and WMI's PnP devices (wbem.cpp), which games
// match up to find which DirectInput devices are XInput ones.

#include <cstdint>
#include <string>
#include <vector>

namespace recomp::hle {

struct XInputPad {
  uint32_t index;     // XInput user index
  uint32_t id;        // SDL instance id: tells a pad from its successor at the same index
  uint16_t vid, pid;  // USB vendor and product id
  std::string name;
};

// A pad's controls as SDL reports them, indexed by SDL_GamepadAxis (sticks
// -32768..32767, down positive; triggers 0..32767) and SDL_GamepadButton.
struct PadState {
  int16_t axis[6];
  bool button[32];
};

// The connected pads in user index order. Refreshes the pad list on the
// main thread (elsewhere: the pads already open).
std::vector<XInputPad> XInputPads();

// Reads the pad at `index` if it is still the one with SDL instance `id`.
bool XInputPadRead(uint32_t index, uint32_t id, PadState* out);

}  // namespace recomp::hle
