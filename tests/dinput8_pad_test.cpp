// dinput8's game controller on an SDL virtual gamepad, through the thunk
// dispatch: enumeration, the c_dfDIJoystick2 state with the ranges a game
// sets, the POV, action maps and their buffered events, unsupported force
// feedback, and unplugging.

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "runtime/ctx.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/runtime.h"

using namespace recomp;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", what);
    failures++;
  }
}

constexpr uint32_t kStack = 0x00300000, kScratch = 0x00310000;
constexpr uint32_t DIERR_UNSUPPORTED = 0x80004001, DIERR_UNPLUGGED = 0x80040209,
                   DIERR_INVALIDPARAM = 0x80070057;
Ctx g_ctx;

struct Instance {
  uint8_t guid[16];
  uint32_t devtype;
  std::string product;
};
struct Object {
  uint32_t ofs, type, flags;
};
std::vector<Instance> g_instances;
std::vector<Object> g_objects;

HLE_NAMED("dinput8test.dll", "EnumDevicesCb", TestEnumDevicesCb, 2) {
  const uint8_t* di = GPtr(hle::Arg(c, 0));
  Instance i;
  std::memcpy(i.guid, di + 4, 16);
  std::memcpy(&i.devtype, di + 0x24, 4);
  i.product = reinterpret_cast<const char*>(di + 0x12C);
  // Real pads on the test host are listed too: keep the keyboard and the virtual pad.
  if ((i.devtype & 0xFF) != 0x15 || i.product == "Virtual DualSense") g_instances.push_back(i);
  return 1;
}

HLE_NAMED("dinput8test.dll", "EnumObjectsCb", TestEnumObjectsCb, 2) {
  const uint8_t* o = GPtr(hle::Arg(c, 0));
  Object ob;
  std::memcpy(&ob.ofs, o + 0x14, 4);
  std::memcpy(&ob.type, o + 0x18, 4);
  std::memcpy(&ob.flags, o + 0x1C, 4);
  g_objects.push_back(ob);
  return 1;
}

// Calls a stdcall guest-visible function: args pushed right to left.
uint32_t CallAt(gaddr thunk, std::vector<uint32_t> args) {
  gaddr sp = kStack + 0x8000;
  for (size_t i = args.size(); i-- > 0;) {
    sp -= 4;
    g_proc->mem.write<uint32_t>(sp, args[i]);
  }
  sp -= 4;
  g_proc->mem.write<uint32_t>(sp, 0x1234);
  g_ctx.esp = uint32_t(sp);
  hle::CallThunk(g_ctx, thunk);
  return g_ctx.eax;
}

uint32_t Call(const char* name, std::vector<uint32_t> args) {
  return CallAt(hle::ImportThunk("dinput8.dll", name), args);
}

uint32_t Rd(uint32_t va) { return g_proc->mem.read<uint32_t>(va); }
void Wr(uint32_t va, uint32_t v) { g_proc->mem.write<uint32_t>(va, v); }

int32_t Axis(uint32_t state, int a) { return int32_t(Rd(state + 4 * a)); }

enum Ofs { LX = 0, LY, LZ, LRX, LRY, LRZ };

constexpr uint32_t kCooperativeFg = 0x14, kRange = 4, kById = 2;
constexpr uint32_t kIfc = kScratch, kDev = kScratch + 4, kDev2 = kScratch + 8, kGuid = kScratch + 0x10,
                kFormat = kScratch + 0x40, kCaps = kScratch + 0x80, kState = kScratch + 0x100,
                kRangeProp = kScratch + 0x300;

void SetRange(uint32_t dev, uint32_t obj, uint32_t how, int32_t lo, int32_t hi) {
  Wr(kRangeProp, 24);
  Wr(kRangeProp + 4, 16);
  Wr(kRangeProp + 8, obj);
  Wr(kRangeProp + 12, how);
  Wr(kRangeProp + 16, uint32_t(lo));
  Wr(kRangeProp + 20, uint32_t(hi));
  Check(Call("IDirectInputDevice8A::SetProperty", {dev, kRange, kRangeProp}) == 0, "SetProperty range");
}

uint32_t OpenDevice(uint32_t out) {
  Check(Call("IDirectInput8A::CreateDevice", {Rd(kIfc), kGuid, out, 0}) == 0, "CreateDevice");
  uint32_t dev = Rd(out);
  Wr(kFormat + 12, 272);
  Check(Call("IDirectInputDevice8A::SetDataFormat", {dev, kFormat}) == 0, "SetDataFormat");
  Check(Call("IDirectInputDevice8A::SetCooperativeLevel", {dev, 0, kCooperativeFg}) == 0, "SetCooperativeLevel");
  return dev;
}

// An action format at kActionFormat: one DIACTION (40 bytes) a semantic,
// uAppData its index; range -100..100, 32 events buffered.
constexpr uint32_t kActionFormat = kScratch + 0x400, kActions = kScratch + 0x500, kEvents = kScratch + 0x800,
                   kCount = kScratch + 0xC00;
void WriteFormat(uint32_t genre, const std::vector<uint32_t>& semantics) {
  std::memset(GPtr(kActionFormat), 0, 0x100);
  std::memset(GPtr(kActions), 0, 40 * semantics.size());
  Wr(kActionFormat, 0x14C);
  Wr(kActionFormat + 4, 40);
  Wr(kActionFormat + 8, 4 * uint32_t(semantics.size()));
  Wr(kActionFormat + 12, uint32_t(semantics.size()));
  Wr(kActionFormat + 16, kActions);
  Wr(kActionFormat + 0x24, genre);
  Wr(kActionFormat + 0x28, 32);
  Wr(kActionFormat + 0x2C, uint32_t(-100));
  Wr(kActionFormat + 0x30, 100);
  for (size_t i = 0; i < semantics.size(); i++) {
    Wr(kActions + 40 * uint32_t(i), uint32_t(i));
    Wr(kActions + 40 * uint32_t(i) + 4, semantics[i]);
  }
}
uint32_t ActionHow(int i) { return Rd(kActions + 40 * i + 36); }
uint32_t ActionObj(int i) { return Rd(kActions + 40 * i + 32); }

// The buffered events since the last call: uAppData -> the last value.
std::vector<std::pair<uint32_t, int32_t>> Events(uint32_t dev) {
  Wr(kCount, 32);
  Check(Call("IDirectInputDevice8A::GetDeviceData", {dev, 20, kEvents, kCount, 0}) == 0, "GetDeviceData");
  std::vector<std::pair<uint32_t, int32_t>> out;
  for (uint32_t i = 0; i < Rd(kCount); i++) out.push_back({Rd(kEvents + 20 * i + 16), int32_t(Rd(kEvents + 20 * i + 4))});
  return out;
}

void Press(SDL_Joystick* j, SDL_GamepadButton b, bool down) {
  SDL_SetJoystickVirtualButton(j, b, down);
  SDL_UpdateJoysticks();
}

void Push(SDL_Joystick* j, SDL_GamepadAxis a, int v) {
  SDL_SetJoystickVirtualAxis(j, a, int16_t(v));
  SDL_UpdateJoysticks();
}

}  // namespace

int main() {
  static Process proc;
  g_proc = &proc;
  if (!proc.mem.Reserve()) return 1;
  proc.vm.Init(&proc.mem, 0x10000, 0x70000000);
  proc.mem.Commit(kPebVa & ~0xFFFu, 0x2000, Prot::RW);
  proc.mem.Commit(kStack, 0x20000, Prot::RW);
  proc.mem.Commit(kScratch, 0x10000, Prot::RW);
  InitKernel32State();

  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  if (!SDL_Init(SDL_INIT_GAMEPAD)) return 1;
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
  desc.name = "Virtual DualSense";
  SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
  SDL_Joystick* joy = id ? SDL_OpenJoystick(id) : nullptr;
  if (!joy || !SDL_IsGamepad(id)) {
    std::fprintf(stderr, "no virtual gamepad: %s\n", SDL_GetError());
    return 1;
  }
  SDL_UpdateJoysticks();

  const gaddr unk = kScratch + 0x1000;
  uint8_t iid[16] = {0x30, 0x80, 0x79, 0xBF, 0x3A, 0x48, 0xA2, 0x4D, 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00};
  std::memcpy(GPtr(unk), iid, 16);
  Check(CallAt(hle::ImportThunk("dinput8.dll", "DirectInput8Create"), {0x400000, 0x800, unk, kIfc, 0}) == 0,
        "DirectInput8Create");

  const gaddr enum_devices = hle::ImportThunk("dinput8test.dll", "EnumDevicesCb");
  const gaddr enum_objects = hle::ImportThunk("dinput8test.dll", "EnumObjectsCb");
  Check(Call("IDirectInput8A::EnumDevices", {Rd(kIfc), 4, uint32_t(enum_devices), 0, 1}) == 0, "EnumDevices");
  Check(g_instances.size() == 1 && (g_instances[0].devtype & 0xFF) == 0x15, "one gamepad enumerated");
  g_instances.clear();
  Check(Call("IDirectInput8A::EnumDevices", {Rd(kIfc), 3, uint32_t(enum_devices), 0, 1}) == 0 &&
            g_instances.size() == 1 && (g_instances[0].devtype & 0xFF) == 0x13,
        "the keyboard class lists no pad");
  g_instances.clear();
  Call("IDirectInput8A::EnumDevices", {Rd(kIfc), 4, uint32_t(enum_devices), 0, 1});
  std::memcpy(GPtr(kGuid), g_instances[0].guid, 16);

  uint32_t dev = OpenDevice(kDev);
  Check(Call("IDirectInputDevice8A::GetCapabilities", {dev, kCaps}) == DIERR_INVALIDPARAM, "GetCapabilities size check");
  Wr(kCaps, 44);
  Check(Call("IDirectInputDevice8A::GetCapabilities", {dev, kCaps}) == 0, "GetCapabilities");
  Check((Rd(kCaps + 4) & 0x100) == 0, "no force feedback flag");
  Check(Rd(kCaps + 12) == 6 && Rd(kCaps + 16) == 14 && Rd(kCaps + 20) == 1, "axes, buttons, POVs");

  Check(Call("IDirectInputDevice8A::EnumObjects", {dev, uint32_t(enum_objects), 0, 0}) == 0, "EnumObjects");
  Check(g_objects.size() == 6 + 1 + 14, "all objects");
  std::vector<Object> axes;
  for (const Object& o : g_objects) {
    Check((o.flags & 1) == 0, "no FF actuator");
    if (o.type & 2) axes.push_back(o);
  }
  Check(axes.size() == 6, "six axes");
  g_objects.clear();
  Call("IDirectInputDevice8A::EnumObjects", {dev, uint32_t(enum_objects), 0, 3});
  Check(g_objects.size() == 6, "DIDFT_AXIS filter");

  // The ranges a game sets from the enumeration.
  for (const Object& o : axes) SetRange(dev, o.type, kById, -1000, 1000);

  Check(Call("IDirectInputDevice8A::GetDeviceState", {dev, 272, kState}) == 0x8007000C, "not acquired");
  Check(Call("IDirectInputDevice8A::Acquire", {dev}) == 0, "Acquire");
  Check(Call("IDirectInputDevice8A::Poll", {dev}) == 1, "Poll");
  Check(Call("IDirectInputDevice8A::GetDeviceState", {dev, 256, kState}) == DIERR_INVALIDPARAM, "wrong state size");
  Check(Call("IDirectInputDevice8A::GetDeviceState", {dev, 272, kState}) == 0, "GetDeviceState");
  Check(Axis(kState, LX) == 0 && Axis(kState, LY) == 0 && Axis(kState, LZ) == 0 && Axis(kState, LRZ) == 0,
        "sticks centred");
  Check(Axis(kState, LRX) == -1000 && Axis(kState, LRY) == -1000, "triggers rest at the low end");
  Check(Rd(kState + 32) == 0xFFFFFFFF, "POV centred");
  bool none = true;
  for (int b = 0; b < 128; b++) none &= g_proc->mem.read<uint8_t>(kState + 48 + b) == 0;
  Check(none, "no buttons");

  Push(joy, SDL_GAMEPAD_AXIS_LEFTX, 32767);
  Push(joy, SDL_GAMEPAD_AXIS_LEFTY, -32768);
  Push(joy, SDL_GAMEPAD_AXIS_RIGHTX, -32768);
  Push(joy, SDL_GAMEPAD_AXIS_RIGHTY, 32767);
  Push(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
  Push(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -16000);  // a virtual trigger axis spans -32768..32767
  Call("IDirectInputDevice8A::GetDeviceState", {dev, 272, kState});
  Check(Axis(kState, LX) == 1000, "left stick right is +1000");
  Check(Axis(kState, LY) == -1000, "left stick up is -1000 (Y grows down)");
  Check(Axis(kState, LZ) == -1000, "right stick left");
  Check(Axis(kState, LRZ) == 1000, "right stick down");
  Check(Axis(kState, LRY) == 1000, "right trigger full");
  Check(Axis(kState, LRX) > -500 && Axis(kState, LRX) < -450, "left trigger a quarter way");
  auto button = [&](int b) { return g_proc->mem.read<uint8_t>(kState + 48 + b); };
  Check(button(7) == 0x80 && button(6) == 0, "R2 button past half, L2 not");

  Press(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
  Press(joy, SDL_GAMEPAD_BUTTON_START, true);
  Press(joy, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
  Press(joy, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
  Call("IDirectInputDevice8A::GetDeviceState", {dev, 272, kState});
  Check(button(1) == 0x80 && button(9) == 0x80 && button(0) == 0 && button(2) == 0, "cross, options");
  Check(Rd(kState + 32) == 4500, "D-pad up-right is 45 degrees");
  Press(joy, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
  Press(joy, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false);
  Press(joy, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
  Call("IDirectInputDevice8A::GetDeviceState", {dev, 272, kState});
  Check(Rd(kState + 32) == 27000, "D-pad left");

  // A device that sets no range reports the DirectInput default, 0..65535.
  uint32_t dev2 = OpenDevice(kDev2);
  Call("IDirectInputDevice8A::Acquire", {dev2});
  Call("IDirectInputDevice8A::GetDeviceState", {dev2, 272, kState});
  Check(Axis(kState, LX) == 65535 && Axis(kState, LY) == 0, "default range ends");
  Push(joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
  Call("IDirectInputDevice8A::GetDeviceState", {dev2, 272, kState});
  Check(Axis(kState, LX) == 32768, "default range centre");
  SetRange(dev2, 0, 0, -10000, 10000);
  Call("IDirectInputDevice8A::GetDeviceState", {dev2, 272, kState});
  Check(Axis(kState, LX) == 0 && Axis(kState, LRX) > -5200 && Axis(kState, LRX) < -4800, "range of the whole device");

  Check(Call("IDirectInputDevice8A::SendForceFeedbackCommand", {dev, 0x10}) == DIERR_UNSUPPORTED,
        "force feedback is unsupported");

  // A driving action map: steer, accelerate, brake, the stick's Y (left
  // unmapped while the stick steers), DIBUTTON_ANY(0) moved to cross by the game's
  // pad_buttons, ANY(3), the menu button, a keyboard semantic, ANY(2)
  // left out by pad_buttons.
  Press(joy, SDL_GAMEPAD_BUTTON_DPAD_LEFT, false);
  Push(joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
  Push(joy, SDL_GAMEPAD_AXIS_LEFTY, 0);
  Push(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);  // a virtual trigger's rest
  Push(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
  Press(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
  Press(joy, SDL_GAMEPAD_BUTTON_START, false);
  proc.game.pad_buttons = {{0xFF004400, SDL_GAMEPAD_BUTTON_SOUTH}, {0xFF004402, -1}};
  uint32_t mdev = OpenDevice(kDev2 + 0x20);
  WriteFormat(0x01000000, {0x01008A01, 0x01039202, 0x01041203, 0xFF014201, 0xFF004400, 0xFF004403, 0x010004FD,
                           0x810000C8, 0xFF004402});
  Check(Call("IDirectInputDevice8A::BuildActionMap", {mdev, kActionFormat, 0, 0}) == 0, "BuildActionMap");
  Check(ActionObj(0) == 0x002 && ActionObj(1) == 0x402 && ActionObj(2) == 0x302, "steer X, accelerate Ry, brake Rx");
  Check(ActionHow(3) == 0 && ActionHow(7) == 0 && ActionHow(8) == 0,
        "no stick Y while it steers, no keyboard semantic on a pad, ANY(2) unmapped by pad_buttons");
  Check(ActionObj(4) == 0x104 && ActionObj(5) == 0x304 && ActionObj(6) == 0x904,
        "ANY(0) on cross (pad_buttons), ANY(3) the pad's button 3, menu on options");
  Check(ActionHow(0) == 0x20 && std::memcmp(GPtr(kActions + 16), g_instances[0].guid, 16) == 0,
        "mapped to this pad");
  Check(Call("IDirectInputDevice8A::SetActionMap", {mdev, kActionFormat, 0, 0}) == 0, "SetActionMap");
  Check(Call("IDirectInputDevice8A::Acquire", {mdev}) == 0, "Acquire with an action map");
  Check(Events(mdev).empty(), "at rest: no events");
  Push(joy, SDL_GAMEPAD_AXIS_LEFTX, 32767);
  Push(joy, SDL_GAMEPAD_AXIS_LEFTY, 32767);
  Push(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
  Push(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);  // half way
  Press(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
  Press(joy, SDL_GAMEPAD_BUTTON_START, true);
  auto ev = Events(mdev);
  std::map<uint32_t, int32_t> got(ev.begin(), ev.end());
  Check(ev.size() == 5 && got[0] == 100 && got[1] == -100 && got[2] == -50 && got[4] == 0x80 && got[6] == 0x80,
        "steer right 100, triggers from 0 down to -100, buttons 0x80");
  Check(Events(mdev).empty(), "unchanged: no events");
  Push(joy, SDL_GAMEPAD_AXIS_LEFTX, 3000);
  ev = Events(mdev);
  Check(ev.size() == 1 && ev[0].first == 0 && ev[0].second == 0, "a stick inside the dead zone: centred");
  Push(joy, SDL_GAMEPAD_AXIS_LEFTX, -32768);
  ev = Events(mdev);
  Check(ev.size() == 1 && ev[0].second == -100, "past it: the full range");
  Press(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
  ev = Events(mdev);
  Check(ev.size() == 1 && ev[0].first == 4 && ev[0].second == 0, "a button let go");
  Call("IDirectInputDevice8A::Unacquire", {mdev});

  // A menu map in the driving genre (no steering): the stick's Y maps,
  // and the D-pad drives DIAXIS_ANY_X_1 and _Y_1 to their ends.
  WriteFormat(0x01000000, {0xFF00C201, 0xFF014201});
  Call("IDirectInputDevice8A::BuildActionMap", {mdev, kActionFormat, 0, 0});
  Check(ActionObj(0) == 0x002 && ActionObj(1) == 0x102, "menus: ANY_X_1 on X, ANY_Y_1 on Y");
  Call("IDirectInputDevice8A::SetActionMap", {mdev, kActionFormat, 0, 0});
  Call("IDirectInputDevice8A::Acquire", {mdev});
  Push(joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
  Push(joy, SDL_GAMEPAD_AXIS_LEFTY, 0);
  Press(joy, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
  ev = Events(mdev);
  Check(ev.size() == 1 && ev[0].first == 0 && ev[0].second == -100, "D-pad left: ANY_X_1 at -100");
  Press(joy, SDL_GAMEPAD_BUTTON_DPAD_LEFT, false);
  Press(joy, SDL_GAMEPAD_BUTTON_START, false);
  Push(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
  Push(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);

  SDL_DetachVirtualJoystick(id);
  SDL_UpdateJoysticks();
  Check(Call("IDirectInputDevice8A::GetDeviceState", {dev, 272, kState}) == DIERR_UNPLUGGED, "unplugged state");
  Check(Call("IDirectInputDevice8A::Poll", {dev}) == DIERR_UNPLUGGED, "unplugged poll");

  for (uint32_t d : {dev, dev2, mdev, Rd(kIfc)})
    Call(d == Rd(kIfc) ? "IDirectInput8A::Release" : "IDirectInputDevice8A::Release", {d});

  if (!failures) std::puts("dinput8 pad OK");
  std::fflush(nullptr);
  std::_Exit(failures ? 1 : 0);  // the guest heap's destructor locks a mutex that is already gone at exit
}
