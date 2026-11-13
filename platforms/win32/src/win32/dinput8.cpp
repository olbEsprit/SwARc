// dinput8: DirectInput8Create, IDirectInput8A and IDirectInputDevice8A for
// the system keyboard and mouse.
//
// Input comes from the event pump's raw input (input_state.h): the
// keyboard by DIK code, the mouse in mickeys. Each acquired device keeps
// its own state and, with DIPROP_BUFFERSIZE set, a buffer of events.
// The XInput pads (SDL gamepads) are game controllers with the layout of
// a DualSense under Windows' DirectInput driver: c_dfDIJoystick2 state,
// read from SDL when asked, or action maps whose buffered events are the
// changes seen at each GetDeviceData. They have no force feedback.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>

#include "runtime/gfx/overlay.h"
#include "runtime/log.h"
#include "win32/com.h"
#include "win32/hle.h"
#include "win32/input_state.h"
#include "win32/kernel32_state.h"
#include "win32/user32_window.h"
#include "win32/xinput.h"

namespace recomp::hle {
namespace {

using guest::GUID;

constexpr GUID kGUID_SysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kGUID_SysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIID_IDirectInput8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
constexpr GUID kIID_IDirectInputDevice8A = {0x54D41080, 0xDC15, 0x4833, {0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79}};

constexpr uint32_t DI_OK = 0;
constexpr uint32_t DI_BUFFEROVERFLOW = 1;
constexpr uint32_t DI_NOEFFECT = 1;
constexpr uint32_t DIERR_INVALIDPARAM = 0x80070057;
constexpr uint32_t DIERR_NOINTERFACE = 0x80004002;
constexpr uint32_t DIERR_UNSUPPORTED = 0x80004001;
constexpr uint32_t DIERR_DEVICENOTREG = 0x80040154;
constexpr uint32_t DIERR_NOTACQUIRED = 0x8007000C;
constexpr uint32_t DIERR_OBJECTNOTFOUND = 0x80070002;
constexpr uint32_t DIERR_UNPLUGGED = 0x80040209;
constexpr uint32_t DIERR_ACQUIRED = 0x800700AA;

// Device classes (EnumDevices) and types.
constexpr uint32_t DI8DEVCLASS_ALL = 0, DI8DEVCLASS_DEVICE = 1,
                   DI8DEVCLASS_POINTER = 2, DI8DEVCLASS_KEYBOARD = 3,
                   DI8DEVCLASS_GAMECTRL = 4;
constexpr uint32_t DI8DEVTYPE_MOUSE = 0x12, DI8DEVTYPE_KEYBOARD = 0x13,
                   DI8DEVTYPE_GAMEPAD = 0x15;
constexpr uint32_t kMouseType = DI8DEVTYPE_MOUSE | (2 << 8);        // traditional
constexpr uint32_t kKeyboardType = DI8DEVTYPE_KEYBOARD | (4 << 8);  // PC enhanced
constexpr uint32_t kGamepadType = DI8DEVTYPE_GAMEPAD | (2 << 8) | 0x10000;  // standard, HID

constexpr uint32_t DISCL_EXCLUSIVE = 0x1;
constexpr uint32_t DIGDD_PEEK = 0x1;
constexpr uint32_t DIPROP_BUFFERSIZE = 1, DIPROP_AXISMODE = 2, DIPROP_RANGE = 4;
constexpr uint32_t DIPH_DEVICE = 0, DIPH_BYOFFSET = 1, DIPH_BYID = 2;
constexpr uint32_t DIDFT_ABSAXIS = 0x2, DIDFT_PSHBUTTON = 0x4, DIDFT_POV = 0x10;
constexpr uint32_t DIPROPAXISMODE_ABS = 0;
constexpr uint32_t DIDC_ATTACHED = 0x1;

// DIJOYSTATE2 (c_dfDIJoystick2): six axes at 0.., two sliders, four POVs at
// 32, 128 buttons at 48, then velocity/acceleration/force axes (unused).
constexpr uint32_t kJoyState2Size = 272, kJoyAxes = 6, kJoyButtons = 14, kJoyPOVOfs = 32,
                   kJoyButtonOfs = 48;

// Mouse data offsets (DIMOUSESTATE / DIMOUSESTATE2).
constexpr uint32_t DIMOFS_X = 0, DIMOFS_Y = 4, DIMOFS_Z = 8, DIMOFS_BUTTON0 = 12;

struct DIDEVCAPS {
  uint32_t dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs,
      dwFFSamplePeriod, dwFFMinTimeResolution, dwFirmwareRevision,
      dwHardwareRevision, dwFFDriverVersion;
};
static_assert(sizeof(DIDEVCAPS) == 44);

struct DIDEVICEINSTANCEA {
  uint32_t dwSize;
  GUID guidInstance, guidProduct;
  uint32_t dwDevType;
  char tszInstanceName[260], tszProductName[260];
  GUID guidFFDriver;
  uint16_t wUsagePage, wUsage;
};
static_assert(sizeof(DIDEVICEINSTANCEA) == 580);

// DIDEVICEOBJECTDATA; DirectX 3 apps pass the 16-byte form without uAppData.
struct DIDEVICEOBJECTDATA {
  uint32_t dwOfs, dwData, dwTimeStamp, dwSequence, uAppData;
};

// DIACTIONA and the head of DIACTIONFORMATA (action maps).
struct DIDEVICEOBJECTINSTANCEA {
  uint32_t dwSize;
  GUID guidType;
  uint32_t dwOfs, dwType, dwFlags;
  char tszName[260];
  uint32_t dwFFMaxForce, dwFFForceResolution;
  uint16_t wCollectionNumber, wDesignatorIndex, wUsagePage, wUsage;
  uint32_t dwDimension;
  uint16_t wExponent, wReserved;
};
static_assert(sizeof(DIDEVICEOBJECTINSTANCEA) == 316);

struct DIPROPRANGE {
  uint32_t dwSize, dwHeaderSize, dwObj, dwHow;
  int32_t lMin, lMax;
};

struct DIACTIONA {
  uint32_t uAppData, dwSemantic, dwFlags, lptszActionName;
  GUID guidInstance;
  uint32_t dwObjID, dwHow;
};
static_assert(sizeof(DIACTIONA) == 40);

struct DIACTIONFORMATA {
  uint32_t dwSize, dwActionSize, dwDataSize, dwNumActions, rgoAction;
  GUID guidActionMap;
  uint32_t dwGenre, dwBufferSize;
  int32_t lAxisMin, lAxisMax;
};

constexpr uint32_t DIAH_UNMAPPED = 0, DIAH_DEFAULT = 0x20;
constexpr uint32_t DIDFT_BUTTON = 0x0C;

struct Direct8 : com::Object {};

struct InputDevice : com::Object {
  bool keyboard = false;
  bool pad = false;
  XInputPad pad_info;
  struct Range {
    int32_t lo = 0, hi = 65535;
  } range[kJoyAxes];
  uint32_t data_size = 0;  // the data format's state size; 0 = not set
  uint32_t coop = 0;
  uint32_t hwnd = 0;
  bool acquired = false;
  bool grabbed = false;  // exclusive mouse: host pointer in relative mode
  uint32_t buffer_size = 0;
  bool overflow = false;
  std::deque<DIDEVICEOBJECTDATA> buffer;
  // Mouse: absolute position (DIPROPAXISMODE_ABS) or motion since the last
  // GetDeviceState, wheel, buttons.
  bool absolute = false;
  int32_t axis[3] = {};
  uint8_t buttons[8] = {};
  // Keyboard: DIK state; with an action map, the uAppData of each mapped key.
  uint8_t keys[256] = {};
  bool mapped = false;
  std::map<uint8_t, uint32_t> actions;
  // Pad with an action map: each mapped action's object (dwObjID), the
  // value last reported, and the format's axis range.
  struct PadAction {
    uint32_t app, semantic, obj;
    int32_t last = 0;
    bool dpad_axes = true;  // the D-pad also moves DIAXIS_ANY_X_1/_Y_1, unless the map gives it actions of its own
  };
  std::vector<PadAction> pad_actions;
  int32_t axis_lo = 0, axis_hi = 0;
  ~InputDevice() override;
};

std::mutex g_mu;
std::set<InputDevice*> g_acquired;  // devices that get raw input
uint32_t g_sequence = 0;

void Record(InputDevice& d, uint32_t ofs, uint32_t data, uint32_t time, uint32_t app = 0) {
  if (!d.buffer_size) return;
  if (d.buffer.size() >= d.buffer_size) {
    d.buffer.pop_front();
    d.overflow = true;
  }
  d.buffer.push_back({ofs, data, time, ++g_sequence, app});
}

void OnRawInput(const RawInput& in) {
  std::lock_guard<std::mutex> l(g_mu);
  for (InputDevice* d : g_acquired) {
    if (d->keyboard) {
      if (in.kind != RawInput::Key) continue;
      uint8_t v = in.value ? 0x80 : 0;
      if (d->keys[in.code] == v) continue;
      d->keys[in.code] = v;
      if (!d->mapped) {
        Record(*d, in.code, v, in.time_ms);
      } else if (auto it = d->actions.find(uint8_t(in.code)); it != d->actions.end()) {
        Record(*d, in.code, v, in.time_ms, it->second);
      }
      continue;
    }
    switch (in.kind) {
      case RawInput::MouseX:
      case RawInput::MouseY:
      case RawInput::Wheel: {
        int a = in.kind == RawInput::MouseX ? 0 : in.kind == RawInput::MouseY ? 1 : 2;
        d->axis[a] += in.value;
        // Buffered axis data is the motion (relative) or the position.
        uint32_t data = uint32_t(d->absolute ? d->axis[a] : in.value);
        Record(*d, a == 0 ? DIMOFS_X : a == 1 ? DIMOFS_Y : DIMOFS_Z, data, in.time_ms);
        break;
      }
      case RawInput::MouseButton:
        if (in.code < 8) {
          d->buttons[in.code] = in.value ? 0x80 : 0;
          Record(*d, DIMOFS_BUTTON0 + in.code, d->buttons[in.code], in.time_ms);
        }
        break;
      default:
        break;
    }
  }
}

// The exclusive mouse hides the pointer and reports motion without
// bounds, as on Windows: SDL's relative mode on the device's window.
void Grab(InputDevice& d, bool on) {
  if (d.grabbed == on || d.keyboard || d.pad || !(d.coop & DISCL_EXCLUSIVE) || Background())
    return;
  if (SDL_Window* w = SdlWindowFor(d.hwnd)) SDL_SetWindowRelativeMouseMode(w, on);
  d.grabbed = on;
}

void Unacquire(InputDevice& d) {
  {
    std::lock_guard<std::mutex> l(g_mu);
    g_acquired.erase(&d);
  }
  if (d.acquired) Grab(d, false);
  d.acquired = false;
}

InputDevice::~InputDevice() { Unacquire(*this); }

uint32_t DirectInputVtable() {
  return com::Vtable("dinput8.dll", "IDirectInput8A",
                     {"QueryInterface", "AddRef", "Release", "CreateDevice",
                      "EnumDevices", "GetDeviceStatus", "RunControlPanel",
                      "Initialize", "FindDevice", "EnumDevicesBySemantics",
                      "ConfigureDevices"});
}

uint32_t DeviceVtable() {
  return com::Vtable(
      "dinput8.dll", "IDirectInputDevice8A",
      {"QueryInterface", "AddRef", "Release", "GetCapabilities", "EnumObjects",
       "GetProperty", "SetProperty", "Acquire", "Unacquire", "GetDeviceState",
       "GetDeviceData", "SetDataFormat", "SetEventNotification",
       "SetCooperativeLevel", "GetObjectInfo", "GetDeviceInfo",
       "RunControlPanel", "Initialize", "CreateEffect", "EnumEffects",
       "GetEffectInfo", "GetForceFeedbackState", "SendForceFeedbackCommand",
       "EnumCreatedEffectObjects", "Escape", "Poll", "SendDeviceData",
       "EnumEffectsInFile", "WriteEffectToFile", "BuildActionMap",
       "SetActionMap", "GetImageInfo"});
}

InputDevice* Dev(Ctx& c) {
  InputDevice* d = com::As<InputDevice>(Arg(c, 0));
  if (!d) FatalF("IDirectInputDevice8A call on unknown object %08X", uint32_t(Arg(c, 0)));
  return d;
}

constexpr uint32_t kPadGuidData1 = 0x9A6F1D20;

void FillInstance(DIDEVICEINSTANCEA* di, bool keyboard) {
  std::memset(di, 0, sizeof *di);
  di->dwSize = sizeof *di;
  di->guidInstance = di->guidProduct = keyboard ? kGUID_SysKeyboard : kGUID_SysMouse;
  di->dwDevType = keyboard ? kKeyboardType : kMouseType;
  std::snprintf(di->tszInstanceName, sizeof di->tszInstanceName, keyboard ? "Keyboard" : "Mouse");
  std::snprintf(di->tszProductName, sizeof di->tszProductName, keyboard ? "Keyboard" : "Mouse");
  di->wUsagePage = 1;              // generic desktop
  di->wUsage = keyboard ? 6 : 2;   // keyboard / mouse
}

GUID PadInstanceGuid(const XInputPad& pad) {
  return {kPadGuidData1 + pad.index, 0xD5A0, 0x11CF, {'X', 'I', 'N', 'P', 'U', 'T', 0, 0}};
}

void FillPadInstance(DIDEVICEINSTANCEA* di, const XInputPad& pad) {
  std::memset(di, 0, sizeof *di);
  di->dwSize = sizeof *di;
  di->guidInstance = PadInstanceGuid(pad);
  di->guidProduct = {pad.vid | uint32_t(pad.pid) << 16, 0, 0, {0, 0, 'P', 'I', 'D', 'V', 'I', 'D'}};
  di->dwDevType = kGamepadType;
  std::snprintf(di->tszInstanceName, sizeof di->tszInstanceName, "%s", pad.name.c_str());
  std::snprintf(di->tszProductName, sizeof di->tszProductName, "%s", pad.name.c_str());
  di->wUsagePage = 1;  // generic desktop
  di->wUsage = 5;      // game pad
}

// The XInput pad a device instance GUID names (FillPadInstance).
bool PadFromGuid(uint32_t va, XInputPad* out) {
  GUID g;
  std::memcpy(&g, GPtr(va), sizeof g);
  for (const XInputPad& pad : XInputPads()) {
    GUID want = PadInstanceGuid(pad);
    if (std::memcmp(&g, &want, sizeof g) == 0) {
      *out = pad;
      return true;
    }
  }
  return false;
}

// The SDL buttons of c_dfDIJoystick2's buttons; L2 and R2 (-1) are the
// triggers past half way.
// Buttons 14..17, the D-pad, are objects of action maps only: the raw state and the caps keep the D-pad as POV 0.
constexpr uint32_t kActionButtons = kJoyButtons + 4;
constexpr int kPadButtons[kActionButtons] = {
    SDL_GAMEPAD_BUTTON_WEST,          SDL_GAMEPAD_BUTTON_SOUTH,      SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_NORTH,         SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    -1,                               -1,                            SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_START,         SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_GUIDE,         SDL_GAMEPAD_BUTTON_TOUCHPAD,   SDL_GAMEPAD_BUTTON_DPAD_UP,
    SDL_GAMEPAD_BUTTON_DPAD_DOWN,     SDL_GAMEPAD_BUTTON_DPAD_LEFT,  SDL_GAMEPAD_BUTTON_DPAD_RIGHT};

bool PadButtonDown(const PadState& s, uint32_t b) {
  if (b == 6) return s.axis[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] > 16384;
  if (b == 7) return s.axis[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] > 16384;
  return s.button[kPadButtons[b]];
}

// SDL gamepad state as DIJOYSTATE2 on the layout of a DualSense under
// Windows: X/Y the left stick, Z/Rz the right one, Rx/Ry the triggers
// (resting at the range's low end); buttons square, cross, circle, triangle,
// L1, R1, L2, R2, create, options, L3, R3, PS, touchpad; the D-pad as POV 0.
void MapPadState(const PadState& s, const InputDevice::Range (&range)[kJoyAxes], uint8_t* out) {
  std::memset(out, 0, kJoyState2Size);
  auto stick = [](int16_t v) { return int64_t(v) + 32768; };
  auto trigger = [](int16_t v) { return int64_t(std::clamp<int>(v, 0, 32767)) * 65535 / 32767; };
  const int64_t pos[kJoyAxes] = {stick(s.axis[SDL_GAMEPAD_AXIS_LEFTX]),
                                 stick(s.axis[SDL_GAMEPAD_AXIS_LEFTY]),
                                 stick(s.axis[SDL_GAMEPAD_AXIS_RIGHTX]),
                                 trigger(s.axis[SDL_GAMEPAD_AXIS_LEFT_TRIGGER]),
                                 trigger(s.axis[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER]),
                                 stick(s.axis[SDL_GAMEPAD_AXIS_RIGHTY])};
  for (uint32_t a = 0; a < kJoyAxes; a++) {
    int64_t span = int64_t(range[a].hi) - range[a].lo;
    int32_t v = int32_t(range[a].lo + (pos[a] * span + (span >= 0 ? 32767 : -32767)) / 65535);
    std::memcpy(out + 4 * a, &v, 4);
  }
  uint32_t pov[4] = {~0u, ~0u, ~0u, ~0u};
  bool up = s.button[SDL_GAMEPAD_BUTTON_DPAD_UP], down = s.button[SDL_GAMEPAD_BUTTON_DPAD_DOWN],
       left = s.button[SDL_GAMEPAD_BUTTON_DPAD_LEFT], right = s.button[SDL_GAMEPAD_BUTTON_DPAD_RIGHT];
  if (up && !down) pov[0] = right && !left ? 4500 : left && !right ? 31500 : 0;
  else if (down && !up) pov[0] = right && !left ? 13500 : left && !right ? 22500 : 18000;
  else if (right && !left) pov[0] = 9000;
  else if (left && !right) pov[0] = 27000;
  std::memcpy(out + kJoyPOVOfs, pov, sizeof pov);
  for (uint32_t b = 0; b < kJoyButtons; b++) out[kJoyButtonOfs + b] = PadButtonDown(s, b) ? 0x80 : 0;
}

// ---- action maps on pads ------------------------------------------------------

enum JoyAxis : uint32_t { kAxisX, kAxisY, kAxisZ, kAxisRx, kAxisRy, kAxisRz };  // MapPadState's axes

constexpr uint32_t kSemSteer = 0x01008A01, kSemAccelerate = 0x01039202, kSemBrake = 0x01041203,
                   kSemShiftUp = 0x01000C01, kSemShiftDown = 0x01000C02, kSemView = 0x01001C03,
                   kSemBoost = 0x01004408, kSemMenu = 0x010004FD, kSemPause = 0x010044FC,
                   kSemAnyX1 = 0xFF00C201, kSemAnyY1 = 0xFF014201, kSemAnyX2 = 0xFF00C202,
                   kSemAnyY2 = 0xFF014202, kSemAnyButton = 0xFF004400;  // DIBUTTON_ANY(n): | n

constexpr uint32_t AxisObj(uint32_t a) { return DIDFT_ABSAXIS | a << 8; }
constexpr uint32_t ButtonObj(uint32_t b) { return DIDFT_PSHBUTTON | b << 8; }

// The pad object (dwObjID) a semantic maps to; 0 for none. A map that
// steers with the left stick leaves its Y unmapped: it would throttle or
// brake while steering. (Games declare their menus' maps in a driving
// genre too, so the genre does not tell.)
uint32_t PadObjectFor(uint32_t semantic, bool steers) {
  for (const win32::GameConfig::PadButton& p : Game().pad_buttons) {
    if (p.semantic != semantic) continue;
    if (p.button < 0) return 0;
    for (uint32_t b = 0; b < kActionButtons; b++)
      if (kPadButtons[b] == p.button) return ButtonObj(b);
    FatalF("dinput8: pad_buttons names SDL button %d, which is not a pad button", p.button);
  }
  switch (semantic) {
    case kSemSteer: case kSemAnyX1: return AxisObj(kAxisX);
    case kSemAnyY1: return steers ? 0 : AxisObj(kAxisY);
    case kSemAnyX2: return AxisObj(kAxisZ);
    case kSemAnyY2: return AxisObj(kAxisRz);
    case kSemAccelerate: return AxisObj(kAxisRy);  // R2
    case kSemBrake: return AxisObj(kAxisRx);       // L2
    case kSemShiftUp: return ButtonObj(5);         // R1
    case kSemShiftDown: return ButtonObj(4);       // L1
    case kSemView: return ButtonObj(3);            // triangle
    case kSemBoost: return ButtonObj(2);           // circle
    case kSemMenu: case kSemPause: return ButtonObj(9);  // options
  }
  if ((semantic & ~0xFFu) == kSemAnyButton && (semantic & 0xFF) < kJoyButtons) return ButtonObj(semantic & 0xFF);
  return 0;
}

// A mapped action's value: a button 0x80 or 0; an axis in lo..hi, a
// trigger from the centre at rest to lo pressed (as each pedal on a
// combined pedal axis), the D-pad at the ends of DIAXIS_ANY_X_1 and _Y_1
// for menus that take only axes. Sticks get a dead zone, as pad drivers
// give them: raw SDL sticks rest off centre and drift, and games' own dead
// zones are sized for that driver's output.
int32_t PadActionValue(const PadState& s, const InputDevice::PadAction& a, int32_t lo, int32_t hi) {
  const uint32_t i = a.obj >> 8 & 0xFFFF;
  if (a.obj & DIDFT_PSHBUTTON) return PadButtonDown(s, i) ? 0x80 : 0;
  auto trigger = [](int16_t v) { return -std::clamp<int>(v, 0, 32767); };
  static const SDL_GamepadAxis kStick[kJoyAxes] = {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY,
                                                   SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
                                                   SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHTY};
  int64_t v = i == kAxisRx || i == kAxisRy ? trigger(s.axis[kStick[i]]) : s.axis[kStick[i]];
  if (i != kAxisRx && i != kAxisRy) {
    const int64_t kDeadZone = 32767 * gfx::overlay::StickDeadZone() / 100;
    const int64_t m = std::min<int64_t>(std::abs(v), 32767);
    v = m <= kDeadZone ? 0 : (v < 0 ? -1 : 1) * (m - kDeadZone) * 32767 / (32767 - kDeadZone);
  }
  if (a.semantic == kSemAnyX1 && a.dpad_axes) {
    if (s.button[SDL_GAMEPAD_BUTTON_DPAD_LEFT]) v = -32768;
    if (s.button[SDL_GAMEPAD_BUTTON_DPAD_RIGHT]) v = 32767;
  } else if (a.semantic == kSemAnyY1 && a.dpad_axes) {
    if (s.button[SDL_GAMEPAD_BUTTON_DPAD_UP]) v = -32768;
    if (s.button[SDL_GAMEPAD_BUTTON_DPAD_DOWN]) v = 32767;
  }
  return int32_t(lo + ((v + 32768) * (int64_t(hi) - lo) + 32767) / 65535);
}

uint32_t QueryInterfaceFor(Ctx& c, const GUID& iid) {
  if (com::GuidEq(Arg(c, 1), iid) || com::GuidEq(Arg(c, 1), kIID_IUnknown)) {
    com::AddRef(Arg(c, 0));
    Wr32(Arg(c, 2), Arg(c, 0));
    return DI_OK;
  }
  Wr32(Arg(c, 2), 0);
  return DIERR_NOINTERFACE;
}

// (hinst, dwVersion, riidltf, ppvOut, punkOuter)
HLE("dinput8.dll", DirectInput8Create, 5) {
  uint32_t out = Arg(c, 3);
  if (!com::GuidEq(Arg(c, 2), kIID_IDirectInput8A)) {
    Wr32(out, 0);
    return DIERR_NOINTERFACE;
  }
  RECOMP_TRACE("  DirectInput8Create version %X\n", uint32_t(Arg(c, 1)));
  Wr32(out, com::Publish(std::make_shared<Direct8>(), DirectInputVtable()));
  return DI_OK;
}

// ---- IDirectInput8A ----------------------------------------------------------------

HLE_NAMED("dinput8.dll", "IDirectInput8A::QueryInterface", DI8_QueryInterface, 3) {
  return QueryInterfaceFor(c, kIID_IDirectInput8A);
}
HLE_NAMED("dinput8.dll", "IDirectInput8A::AddRef", DI8_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("dinput8.dll", "IDirectInput8A::Release", DI8_Release, 1) { return com::Release(Arg(c, 0)); }

// (rguid, lplpDirectInputDevice, pUnkOuter)
HLE_NAMED("dinput8.dll", "IDirectInput8A::CreateDevice", DI8_CreateDevice, 4) {
  uint32_t out = Arg(c, 2);
  bool keyboard = com::GuidEq(Arg(c, 1), kGUID_SysKeyboard);
  auto d = std::make_shared<InputDevice>();
  if (!keyboard && !com::GuidEq(Arg(c, 1), kGUID_SysMouse)) {
    if (!PadFromGuid(Arg(c, 1), &d->pad_info)) {
      Wr32(out, 0);
      return DIERR_DEVICENOTREG;
    }
    d->pad = true;
  }
  d->keyboard = keyboard;
  Wr32(out, com::Publish(d, DeviceVtable()));
  SetRawInputSink(&OnRawInput);
  return DI_OK;
}

// (dwDevType, lpCallback, pvRef, dwFlags): the keyboard, the mouse and the
// XInput pads, which are all attached.
HLE_NAMED("dinput8.dll", "IDirectInput8A::EnumDevices", DI8_EnumDevices, 5) {
  uint32_t type = Arg(c, 1), cb = Arg(c, 2), ref = Arg(c, 3);
  uint32_t cls = type & 0xFF;
  bool mouse = cls == DI8DEVCLASS_ALL || cls == DI8DEVCLASS_POINTER || cls == DI8DEVTYPE_MOUSE;
  bool keyboard = cls == DI8DEVCLASS_ALL || cls == DI8DEVCLASS_KEYBOARD || cls == DI8DEVTYPE_KEYBOARD;
  bool pads = cls == DI8DEVCLASS_ALL || cls == DI8DEVCLASS_GAMECTRL || cls == DI8DEVTYPE_GAMEPAD;
  if (cls != DI8DEVCLASS_ALL && cls != DI8DEVCLASS_DEVICE && cls != DI8DEVCLASS_POINTER &&
      cls != DI8DEVCLASS_KEYBOARD && cls != DI8DEVCLASS_GAMECTRL && cls < 0x11)
    return DIERR_INVALIDPARAM;
  uint32_t inst = HeapAllocProcess(sizeof(DIDEVICEINSTANCEA), true);
  auto* di = reinterpret_cast<DIDEVICEINSTANCEA*>(GPtr(inst));
  bool more = true;
  for (bool kb : {true, false}) {
    if (!more || (kb ? !keyboard : !mouse)) continue;
    FillInstance(di, kb);
    more = CallGuest(c, cb, {inst, ref});  // 0 = DIENUM_STOP
  }
  if (pads)
    for (const XInputPad& pad : XInputPads()) {
      if (!more) break;
      FillPadInstance(di, pad);
      more = CallGuest(c, cb, {inst, ref});
    }
  HeapFreeProcess(inst);
  return DI_OK;
}

HLE_NAMED("dinput8.dll", "IDirectInput8A::GetDeviceStatus", DI8_GetDeviceStatus, 2) {
  return com::GuidEq(Arg(c, 1), kGUID_SysKeyboard) || com::GuidEq(Arg(c, 1), kGUID_SysMouse)
             ? DI_OK
             : DIERR_DEVICENOTREG;
}

// ---- IDirectInputDevice8A ----------------------------------------------------------

HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::QueryInterface", DID8_QueryInterface, 3) {
  return QueryInterfaceFor(c, kIID_IDirectInputDevice8A);
}
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::AddRef", DID8_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::Release", DID8_Release, 1) { return com::Release(Arg(c, 0)); }

HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::GetCapabilities", DID8_GetCapabilities, 2) {
  InputDevice* d = Dev(c);
  DIDEVCAPS* caps = reinterpret_cast<DIDEVCAPS*>(GPtr(Arg(c, 1)));
  if (caps->dwSize != sizeof(DIDEVCAPS)) return DIERR_INVALIDPARAM;
  std::memset(reinterpret_cast<uint8_t*>(caps) + 4, 0, sizeof *caps - 4);
  caps->dwFlags = DIDC_ATTACHED;
  if (d->pad) {
    caps->dwDevType = kGamepadType;
    caps->dwAxes = kJoyAxes;
    caps->dwButtons = kJoyButtons;
    caps->dwPOVs = 1;
    return DI_OK;
  }
  caps->dwDevType = d->keyboard ? kKeyboardType : kMouseType;
  caps->dwAxes = d->keyboard ? 0 : 3;
  caps->dwButtons = d->keyboard ? 128 : 5;
  return DI_OK;
}

HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::GetDeviceInfo", DID8_GetDeviceInfo, 2) {
  InputDevice* d = Dev(c);
  DIDEVICEINSTANCEA* di = reinterpret_cast<DIDEVICEINSTANCEA*>(GPtr(Arg(c, 1)));
  if (di->dwSize != sizeof(DIDEVICEINSTANCEA)) return DIERR_INVALIDPARAM;
  if (d->pad) FillPadInstance(di, d->pad_info);
  else FillInstance(di, d->keyboard);
  return DI_OK;
}

// (lpdf): the formats are recognised by their state size: c_dfDIKeyboard
// (256), c_dfDIMouse (16), c_dfDIMouse2 (20) and c_dfDIJoystick2 (272).
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::SetDataFormat", DID8_SetDataFormat, 2) {
  InputDevice* d = Dev(c);
  if (d->acquired) return DIERR_ACQUIRED;
  uint32_t size = Rd32(Arg(c, 1) + 12);  // DIDATAFORMAT.dwDataSize
  bool ok = d->pad ? size == kJoyState2Size : d->keyboard ? size == 256 : (size == 16 || size == 20);
  if (!ok) FatalF("dinput8: SetDataFormat with a %u-byte %s format", size,
                  d->pad ? "pad" : d->keyboard ? "keyboard" : "mouse");
  d->data_size = size;
  if (d->pad) d->mapped = false;
  return DI_OK;
}

// (hwnd, dwFlags)
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::SetCooperativeLevel", DID8_SetCooperativeLevel, 3) {
  InputDevice* d = Dev(c);
  d->hwnd = Arg(c, 1);
  d->coop = Arg(c, 2);
  return DI_OK;
}

// DIPROP_RANGE on the device, an axis by offset or by type.
uint32_t SetPadRange(InputDevice& d, const DIPROPRANGE& r) {
  if (r.lMin > r.lMax) return DIERR_INVALIDPARAM;
  uint32_t first = 0, last = kJoyAxes;
  if (r.dwHow == DIPH_BYOFFSET) {
    if (r.dwObj >= 4 * kJoyAxes || r.dwObj % 4) return DIERR_OBJECTNOTFOUND;
    first = r.dwObj / 4, last = first + 1;
  } else if (r.dwHow == DIPH_BYID) {
    if (!(r.dwObj & DIDFT_ABSAXIS) || (r.dwObj >> 8 & 0xFFFF) >= kJoyAxes) return DIERR_OBJECTNOTFOUND;
    first = r.dwObj >> 8 & 0xFFFF, last = first + 1;
  } else if (r.dwHow != DIPH_DEVICE) {
    return DIERR_INVALIDPARAM;
  }
  for (uint32_t a = first; a < last; a++) d.range[a] = {r.lMin, r.lMax};
  return DI_OK;
}

// (rguidProp, pdiph): DIPROPDWORD for the buffer size and axis mode.
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::SetProperty", DID8_SetProperty, 3) {
  InputDevice* d = Dev(c);
  uint32_t prop = Arg(c, 1), ph = Arg(c, 2);
  if (d->pad) {
    if (prop != DIPROP_RANGE) {
      RECOMP_TRACE("  SetProperty %X: unsupported on a pad\n", prop);
      return DIERR_UNSUPPORTED;
    }
    return SetPadRange(*d, *reinterpret_cast<DIPROPRANGE*>(GPtr(ph)));
  }
  uint32_t value = Rd32(ph + 16);  // DIPROPDWORD.dwData
  switch (prop) {
    case DIPROP_BUFFERSIZE: {
      std::lock_guard<std::mutex> l(g_mu);
      d->buffer_size = value;
      while (d->buffer.size() > value) d->buffer.pop_front();
      return DI_OK;
    }
    case DIPROP_AXISMODE:
      if (d->keyboard) return DIERR_UNSUPPORTED;
      d->absolute = value == DIPROPAXISMODE_ABS;
      return DI_OK;
    default:
      RECOMP_TRACE("  SetProperty %X: unsupported\n", prop);
      return DIERR_UNSUPPORTED;
  }
}

HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::GetProperty", DID8_GetProperty, 3) {
  InputDevice* d = Dev(c);
  uint32_t prop = Arg(c, 1), ph = Arg(c, 2);
  switch (prop) {
    case DIPROP_BUFFERSIZE:
      Wr32(ph + 16, d->buffer_size);
      return DI_OK;
    case DIPROP_AXISMODE:
      if (d->keyboard) return DIERR_UNSUPPORTED;
      Wr32(ph + 16, d->absolute ? 0 : 1);
      return DI_OK;
    default:
      RECOMP_TRACE("  GetProperty %X: unsupported\n", prop);
      return DIERR_UNSUPPORTED;
  }
}

// Acquiring starts from the current state: the keys that are down now,
// a mouse at rest (relative mode) or where it was left (absolute mode).
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::Acquire", DID8_Acquire, 1) {
  InputDevice* d = Dev(c);
  if (!d->data_size) return DIERR_INVALIDPARAM;
  if (d->acquired) return DI_NOEFFECT;
  if (d->pad) {
    PadState probe;
    if (!XInputPadRead(d->pad_info.index, d->pad_info.id, &probe)) return DIERR_UNPLUGGED;
    d->acquired = true;
    return DI_OK;
  }
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (d->keyboard) {
      std::memset(d->keys, 0, sizeof d->keys);
      for (int vk = 1; vk < 256; vk++) {
        if (!g_key_down[vk]) continue;
        bool ext;
        uint8_t sc = ScanCodeForVk(uint8_t(vk), &ext);
        if (sc) d->keys[sc | (ext ? 0x80 : 0)] = 0x80;
      }
    }
    g_acquired.insert(d);
  }
  d->acquired = true;
  Grab(*d, true);
  return DI_OK;
}

HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::Unacquire", DID8_Unacquire, 1) {
  InputDevice* d = Dev(c);
  if (!d->acquired) return DI_NOEFFECT;
  Unacquire(*d);
  return DI_OK;
}

// Devices with input of their own are not polled; a pad is read when asked.
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::Poll", DID8_Poll, 1) {
  InputDevice* d = Dev(c);
  if (!d->acquired) return DIERR_NOTACQUIRED;
  PadState probe;
  if (d->pad && !XInputPadRead(d->pad_info.index, d->pad_info.id, &probe)) return DIERR_UNPLUGGED;
  return DI_NOEFFECT;
}

// (cbData, lpvData)
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::GetDeviceState", DID8_GetDeviceState, 3) {
  InputDevice* d = Dev(c);
  uint32_t size = Arg(c, 1), out = Arg(c, 2);
  if (!d->acquired) return DIERR_NOTACQUIRED;
  if (size != d->data_size) return DIERR_INVALIDPARAM;
  if (d->pad) {
    if (d->mapped) FatalF("dinput8: GetDeviceState on a pad with an action map");
    PadState s;
    if (!XInputPadRead(d->pad_info.index, d->pad_info.id, &s)) return DIERR_UNPLUGGED;
    MapPadState(s, d->range, GPtr(out));
    return DI_OK;
  }
  std::lock_guard<std::mutex> l(g_mu);
  if (d->keyboard) {
    std::memcpy(GPtr(out), d->keys, 256);
    return DI_OK;
  }
  for (int a = 0; a < 3; a++) Wr32(out + 4 * uint32_t(a), uint32_t(d->axis[a]));
  if (!d->absolute) std::memset(d->axis, 0, sizeof d->axis);
  std::memcpy(GPtr(out + 12), d->buttons, size - 12);
  return DI_OK;
}

// (cbObjectData, rgdod, pdwInOut, dwFlags): the oldest buffered events;
// a null rgdod flushes them.
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::GetDeviceData", DID8_GetDeviceData, 5) {
  InputDevice* d = Dev(c);
  uint32_t obj_size = Arg(c, 1), out = Arg(c, 2), inout = Arg(c, 3), flags = Arg(c, 4);
  if (!d->acquired) return DIERR_NOTACQUIRED;
  if (!d->buffer_size) return DIERR_INVALIDPARAM;  // DIERR_NOTBUFFERED
  if (obj_size != 16 && obj_size != 20) return DIERR_INVALIDPARAM;
  PadState s;
  if (d->pad && !XInputPadRead(d->pad_info.index, d->pad_info.id, &s)) return DIERR_UNPLUGGED;
  std::lock_guard<std::mutex> l(g_mu);
  if (d->pad && d->mapped) {
    for (InputDevice::PadAction& a : d->pad_actions) {
      int32_t v = PadActionValue(s, a, d->axis_lo, d->axis_hi);
      if (v == a.last) continue;
      a.last = v;
      uint32_t i = a.obj >> 8 & 0xFFFF;
      Record(*d, a.obj & DIDFT_PSHBUTTON ? kJoyButtonOfs + i : 4 * i, uint32_t(v), uint32_t(SDL_GetTicks()), a.app);
    }
  }
  uint32_t n = std::min<uint32_t>(Rd32(inout), uint32_t(d->buffer.size()));
  for (uint32_t i = 0; out && i < n; i++)
    std::memcpy(GPtr(out + i * obj_size), &d->buffer[i], obj_size);
  if (!(flags & DIGDD_PEEK)) d->buffer.erase(d->buffer.begin(), d->buffer.begin() + n);
  Wr32(inout, n);
  bool overflow = d->overflow;
  if (!(flags & DIGDD_PEEK)) d->overflow = false;
  return overflow ? DI_BUFFEROVERFLOW : DI_OK;
}

constexpr GUID kGUID_Axis[kJoyAxes] = {
    {0xA36D02E0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}},
    {0xA36D02E1, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}},
    {0xA36D02E2, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}},
    {0xA36D02F4, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}},
    {0xA36D02F5, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}},
    {0xA36D02E3, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}}};
constexpr GUID kGUID_Button = {0xA36D02F0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kGUID_POV = {0xA36D02F2, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

// (lpCallback, pvRef, dwFlags): the pad's axes, buttons and POV, as
// DIDFT_* types and DIJOYSTATE2 offsets, filtered by dwFlags.
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::EnumObjects", DID8_EnumObjects, 4) {
  InputDevice* d = Dev(c);
  if (!d->pad) FatalF("dinput8: EnumObjects on a keyboard or mouse");
  uint32_t cb = Arg(c, 1), ref = Arg(c, 2), want = Arg(c, 3) & 0xFF;
  uint32_t inst = HeapAllocProcess(sizeof(DIDEVICEOBJECTINSTANCEA), true);
  auto* o = reinterpret_cast<DIDEVICEOBJECTINSTANCEA*>(GPtr(inst));
  static const char* const kAxisNames[kJoyAxes] = {"X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation", "Z Rotation"};
  bool more = true;
  auto emit = [&](const GUID& guid, uint32_t ofs, uint32_t type, uint32_t usage, const char* name) {
    if (!more || (want && !(want & type))) return;
    std::memset(o, 0, sizeof *o);
    o->dwSize = sizeof *o;
    o->guidType = guid;
    o->dwOfs = ofs;
    o->dwType = type;
    o->wUsagePage = 1;
    o->wUsage = uint16_t(usage);
    std::snprintf(o->tszName, sizeof o->tszName, "%s", name);
    more = CallGuest(c, cb, {inst, ref});  // 0 = DIENUM_STOP
  };
  static const uint32_t kAxisUsage[kJoyAxes] = {0x30, 0x31, 0x32, 0x33, 0x34, 0x35};
  for (uint32_t a = 0; a < kJoyAxes; a++)
    emit(kGUID_Axis[a], 4 * a, DIDFT_ABSAXIS | a << 8, kAxisUsage[a], kAxisNames[a]);
  emit(kGUID_POV, kJoyPOVOfs, DIDFT_POV, 0x39, "Hat Switch");
  for (uint32_t b = 0; b < kJoyButtons; b++) {
    char name[16];
    std::snprintf(name, sizeof name, "Button %u", b);
    emit(kGUID_Button, kJoyButtonOfs + b, DIDFT_PSHBUTTON | b << 8, 1, name);
  }
  HeapFreeProcess(inst);
  return DI_OK;
}

// Pads have no force feedback actuators: games take the error and play without.
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::SendForceFeedbackCommand", DID8_SendForceFeedbackCommand, 2) {
  if (!Dev(c)->pad) FatalF("dinput8: SendForceFeedbackCommand on a keyboard or mouse");
  return DIERR_UNSUPPORTED;
}

// (lpdiaf, lpszUserName, dwFlags): the keyboard takes the DIKEYBOARD_*
// semantics, a pad those PadObjectFor gives an object.
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::BuildActionMap", DID8_BuildActionMap, 4) {
  InputDevice* d = Dev(c);
  auto* f = reinterpret_cast<DIACTIONFORMATA*>(GPtr(Arg(c, 1)));
  auto* actions = reinterpret_cast<DIACTIONA*>(GPtr(f->rgoAction));
  bool steers = false;
  for (uint32_t i = 0; i < f->dwNumActions; i++) steers |= actions[i].dwSemantic == kSemSteer;
  // RECOMP_DINPUT_ACTIONS=1 lists the map's actions, to give a game's pad the ones it declares only for keys.
  if (std::getenv("RECOMP_DINPUT_ACTIONS")) {
    WarnF("dinput8: action map genre %08X, %u actions, on the %s\n", f->dwGenre, f->dwNumActions,
          d->keyboard ? "keyboard" : d->pad ? "pad" : "mouse");
    for (uint32_t i = 0; i < f->dwNumActions; i++)
      WarnF("dinput8:   app %08X semantic %08X flags %X \"%s\"\n", actions[i].uAppData, actions[i].dwSemantic,
            actions[i].dwFlags, actions[i].lptszActionName ? reinterpret_cast<const char*>(GPtr(actions[i].lptszActionName)) : "");
  }
  for (uint32_t i = 0; i < f->dwNumActions; i++) {
    DIACTIONA* a = &actions[i];
    if (d->keyboard && (a->dwSemantic & 0xFF000000) == 0x81000000) {
      a->guidInstance = kGUID_SysKeyboard;
      a->dwObjID = (a->dwSemantic & 0xFF) << 8 | DIDFT_BUTTON;
      a->dwHow = DIAH_DEFAULT;
    } else if (uint32_t obj = d->pad && (steers || (a->dwSemantic & 0xFF000000) != 0x81000000)
                                  ? PadObjectFor(a->dwSemantic, steers)
                                  : 0) {
      a->guidInstance = PadInstanceGuid(d->pad_info);
      a->dwObjID = obj;
      a->dwHow = DIAH_DEFAULT;
    } else {
      a->dwHow = DIAH_UNMAPPED;
    }
  }
  return DI_OK;
}

HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::SetActionMap", DID8_SetActionMap, 4) {
  InputDevice* d = Dev(c);
  if (d->acquired) return DIERR_ACQUIRED;
  auto* f = reinterpret_cast<DIACTIONFORMATA*>(GPtr(Arg(c, 1)));
  std::lock_guard<std::mutex> l(g_mu);
  d->actions.clear();
  d->pad_actions.clear();
  const GUID pad = d->pad ? PadInstanceGuid(d->pad_info) : GUID{};
  for (uint32_t i = 0; i < f->dwNumActions; i++) {
    auto* a = reinterpret_cast<DIACTIONA*>(GPtr(f->rgoAction + i * sizeof(DIACTIONA)));
    if (a->dwHow == DIAH_UNMAPPED) continue;
    if (d->keyboard && std::memcmp(&a->guidInstance, &kGUID_SysKeyboard, 16) == 0)
      d->actions[uint8_t(a->dwObjID >> 8)] = a->uAppData;
    if (d->pad && std::memcmp(&a->guidInstance, &pad, 16) == 0) {
      uint32_t n = a->dwObjID >> 8 & 0xFFFF;
      bool axis = (a->dwObjID & DIDFT_ABSAXIS) && n < kJoyAxes, button = (a->dwObjID & DIDFT_PSHBUTTON) && n < kActionButtons;
      if (!axis && !button) FatalF("dinput8: SetActionMap maps a pad action to object 0x%x", a->dwObjID);
      d->pad_actions.push_back({a->uAppData, a->dwSemantic, a->dwObjID});
    }
  }
  if (d->pad) {
    bool dpad = false;
    for (const auto& a : d->pad_actions)
      dpad |= (a.obj & DIDFT_PSHBUTTON) && (a.obj >> 8 & 0xFFFF) >= kJoyButtons;
    for (auto& a : d->pad_actions) a.dpad_axes = !dpad;
    // A format without an axis range leaves DirectInput's default.
    d->axis_lo = f->lAxisMin == f->lAxisMax ? 0 : f->lAxisMin;
    d->axis_hi = f->lAxisMin == f->lAxisMax ? 65535 : f->lAxisMax;
    d->data_size = f->dwDataSize;
  }
  d->mapped = true;
  if (f->dwBufferSize) d->buffer_size = f->dwBufferSize;
  return DI_OK;
}

}  // namespace
}  // namespace recomp::hle
