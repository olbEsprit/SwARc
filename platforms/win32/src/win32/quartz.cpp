// DirectShow (quartz) as far as a game that plays an audio file through
// the filter graph uses it: CLSID_FilterGraph with IGraphBuilder,
// IMediaControl, IMediaSeeking and IBasicAudio. The graph is one decoded
// file on a voice of the software mixer; media time is 100 ns units.

#include <array>
#include <cstdio>
#include <memory>
#include <string>

#include "win32/com.h"
#include "runtime/audio/media_player.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/win32.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

using guest::GUID;

constexpr GUID kCLSID_FilterGraph = {0xE436EBB3, 0x524F, 0x11CE, {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIID_IFilterGraph = {0x56A8689F, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr GUID kIID_IGraphBuilder = {0x56A868A9, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr GUID kIID_IMediaControl = {0x56A868B1, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr GUID kIID_IMediaSeeking = {0x36B73880, 0xC2C8, 0x11CF, {0x8B, 0x46, 0x00, 0x80, 0x5F, 0x6C, 0xEF, 0x60}};
constexpr GUID kIID_IBasicAudio = {0x56A868B3, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};

constexpr uint32_t S_OK = 0, E_NOINTERFACE = 0x80004002, E_POINTER = 0x80004003,
                   E_INVALIDARG = 0x80070057, VFW_E_CANNOT_RENDER = 0x80040217, VFW_E_NOT_FOUND = 0x80040216, VFW_E_NOT_CONNECTED = 0x80040209;

enum Kind { kBuilder, kControl, kSeeking, kAudio, kFilter, kPin, kKinds };

constexpr GUID kTIME_FORMAT_MEDIA_TIME = {0x7B785574, 0x8C82, 0x11CF, {0xBC, 0x0C, 0x00, 0xAA, 0x00, 0xAC, 0x74, 0xF6}};
struct Graph {
  std::array<uint32_t, kKinds> iface{};
  audio::MediaPlayer player;
};

struct Iface : com::Object {
  std::shared_ptr<Graph> graph;
  Kind kind;
  Iface(std::shared_ptr<Graph> g, Kind k) : graph(std::move(g)), kind(k) {}
  ~Iface() override { graph->iface[kind] = 0; }
};

const std::vector<const char*> kUnknown = {"QueryInterface", "AddRef", "Release"};

uint32_t VtableFor(Kind k) {
  std::vector<const char*> m = kUnknown;
  auto add = [&](std::initializer_list<const char*> n) { m.insert(m.end(), n); };
  const char* idisp[] = {"GetTypeInfoCount", "GetTypeInfo", "GetIDsOfNames", "Invoke"};
  switch (k) {
    case kBuilder:
      add({"AddFilter", "RemoveFilter", "EnumFilters", "FindFilterByName",
           "ConnectDirect", "Reconnect", "Disconnect", "SetDefaultSyncSource",
           "Connect", "Render", "RenderFile", "AddSourceFilter", "SetLogFile",
           "Abort", "ShouldOperationContinue"});
      return com::Vtable("quartz.dll", "IGraphBuilder", m);
    case kControl:
      for (const char* n : idisp) add({n});
      add({"Run", "Pause", "Stop", "GetState", "RenderFile", "AddSourceFilter",
           "get_FilterCollection", "get_RegFilterCollection", "StopWhenReady"});
      return com::Vtable("quartz.dll", "IMediaControl", m);
    case kSeeking:
      add({"GetCapabilities", "CheckCapabilities", "IsFormatSupported",
           "QueryPreferredFormat", "GetTimeFormat", "IsUsingTimeFormat",
           "SetTimeFormat", "GetDuration", "GetStopPosition", "GetCurrentPosition",
           "ConvertTimeFormat", "SetPositions", "GetPositions", "GetAvailable",
           "SetRate", "GetRate", "GetPreroll"});
      return com::Vtable("quartz.dll", "IMediaSeeking", m);
    case kFilter:
      add({"GetClassID", "Stop", "Pause", "Run", "GetState", "SetSyncSource",
           "GetSyncSource", "EnumPins", "FindPin", "QueryFilterInfo",
           "JoinFilterGraph", "QueryVendorInfo"});
      return com::Vtable("quartz.dll", "IBaseFilter", m);
    case kPin:
      add({"Connect", "ReceiveConnection", "Disconnect", "ConnectedTo",
           "ConnectionMediaType", "QueryPinInfo", "QueryDirection", "QueryId",
           "QueryAccept", "EnumMediaTypes", "QueryInternalConnections",
           "EndOfStream", "BeginFlush", "EndFlush", "NewSegment"});
      return com::Vtable("quartz.dll", "IPin", m);
    default:
      for (const char* n : idisp) add({n});
      add({"put_Volume", "get_Volume", "put_Balance", "get_Balance"});
      return com::Vtable("quartz.dll", "IBasicAudio", m);
  }
}

uint32_t Interface(const std::shared_ptr<Graph>& g, Kind k) {
  if (g->iface[k]) {
    com::AddRef(g->iface[k]);
    return g->iface[k];
  }
  g->iface[k] = com::Publish(std::make_shared<Iface>(g, k), VtableFor(k));
  return g->iface[k];
}

bool KindOf(uint32_t iid, Kind* k) {
  if (com::GuidEq(iid, kIID_IGraphBuilder) || com::GuidEq(iid, kIID_IFilterGraph)) *k = kBuilder;
  else if (com::GuidEq(iid, kIID_IMediaControl)) *k = kControl;
  else if (com::GuidEq(iid, kIID_IMediaSeeking)) *k = kSeeking;
  else if (com::GuidEq(iid, kIID_IBasicAudio)) *k = kAudio;
  else return false;
  return true;
}

uint32_t CreateFilterGraph(uint32_t iid, uint32_t* out) {
  Kind k;
  if (!KindOf(iid, &k)) {
    *out = 0;
    return E_NOINTERFACE;
  }
  *out = Interface(std::make_shared<Graph>(), k);
  return S_OK;
}

com::CoClassRegistrar reg_filter_graph{kCLSID_FilterGraph, &CreateFilterGraph};

Iface* Self(Ctx& c, const char* iface) {
  Iface* o = com::As<Iface>(Arg(c, 0));
  if (!o) FatalF("%s call on unknown object %08X", iface, uint32_t(Arg(c, 0)));
  return o;
}

uint32_t QueryInterface(Ctx& c, const char* iface) {
  Iface* self = Self(c, iface);
  uint32_t out = Arg(c, 2);
  if (!out) return E_POINTER;
  Kind k = self->kind;
  if (!com::GuidEq(Arg(c, 1), kIID_IUnknown) && !KindOf(Arg(c, 1), &k)) {
    Wr32(out, 0);
    return E_NOINTERFACE;
  }
  Wr32(out, Interface(self->graph, k));
  return S_OK;
}

#define QUARTZ_UNKNOWN(Name)                                                          \
  HLE_NAMED("quartz.dll", #Name "::QueryInterface", Name##_QueryInterface, 3) {      \
    return QueryInterface(c, #Name);                                                  \
  }                                                                                   \
  HLE_NAMED("quartz.dll", #Name "::AddRef", Name##_AddRef, 1) { return com::AddRef(Arg(c, 0)); } \
  HLE_NAMED("quartz.dll", #Name "::Release", Name##_Release, 1) { return com::Release(Arg(c, 0)); }

QUARTZ_UNKNOWN(IGraphBuilder)
QUARTZ_UNKNOWN(IMediaControl)
QUARTZ_UNKNOWN(IMediaSeeking)
QUARTZ_UNKNOWN(IBasicAudio)
QUARTZ_UNKNOWN(IBaseFilter)
QUARTZ_UNKNOWN(IPin)

std::string FileArg(uint32_t p) { return p ? NarrowPath(GWStr(p)) : std::string(); }

uint32_t Load(Graph& g, const std::string& guest_path, const char* what) {
  FILE* f = std::fopen(HostPath(guest_path).c_str(), "rb");
  if (!f) {
    WarnF("quartz: %s(%s): cannot open\n", what, guest_path.c_str());
    return VFW_E_NOT_FOUND;
  }
  auto bytes = std::make_shared<std::vector<uint8_t>>();
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) bytes->insert(bytes->end(), buf, buf + n);
  std::fclose(f);
  std::string err;
  if (!audio::GetMixer().Open(48000)) {
    WarnF("quartz: %s(%s): no audio device\n", what, guest_path.c_str());
    return VFW_E_CANNOT_RENDER;
  }
  if (!g.player.Load(bytes, &err)) {
    WarnF("quartz: %s(%s): %s\n", what, guest_path.c_str(), err.c_str());
    return VFW_E_CANNOT_RENDER;
  }
  return S_OK;
}

// (lpcwstrFileName, lpcwstrFilterName, ppFilter)
HLE_NAMED("quartz.dll", "IGraphBuilder::AddSourceFilter", Builder_AddSourceFilter, 4) {
  Iface* self = Self(c, "IGraphBuilder");
  uint32_t out = Arg(c, 3);
  if (!out) return E_POINTER;
  Wr32(out, 0);
  if (self->graph->player.loaded()) return VFW_E_CANNOT_RENDER;
  uint32_t r = Load(*self->graph, FileArg(Arg(c, 1)), "AddSourceFilter");
  if (r == S_OK) Wr32(out, Interface(self->graph, kFilter));
  return r;
}

// (lpcwstrId, ppPin)
HLE_NAMED("quartz.dll", "IBaseFilter::FindPin", Filter_FindPin, 3) {
  Iface* self = Self(c, "IBaseFilter");
  if (!Arg(c, 2)) return E_POINTER;
  Wr32(Arg(c, 2), 0);
  if (GWStr(Arg(c, 1)) != u"Output") return VFW_E_NOT_FOUND;
  Wr32(Arg(c, 2), Interface(self->graph, kPin));
  return S_OK;
}

// (ppinOut): the output pin of the source renders to the audio device.
HLE_NAMED("quartz.dll", "IGraphBuilder::Render", Builder_Render, 2) {
  Self(c, "IGraphBuilder");
  Iface* pin = com::As<Iface>(Arg(c, 1));
  if (!pin || pin->kind != kPin) FatalF("IGraphBuilder::Render of %08X, not a pin", uint32_t(Arg(c, 1)));
  return S_OK;
}

HLE_NAMED("quartz.dll", "IMediaControl::Run", Control_Run, 1) {
  Graph& g = *Self(c, "IMediaControl")->graph;
  if (g.player.loaded()) g.player.Run();
  return S_OK;
}
HLE_NAMED("quartz.dll", "IMediaControl::Pause", Control_Pause, 1) {
  Graph& g = *Self(c, "IMediaControl")->graph;
  if (g.player.loaded()) g.player.Pause();
  return S_OK;
}
HLE_NAMED("quartz.dll", "IMediaControl::Stop", Control_Stop, 1) {
  Graph& g = *Self(c, "IMediaControl")->graph;
  if (g.player.loaded()) g.player.Stop();
  return S_OK;
}

// (msTimeout, pfs): State_Stopped, State_Paused, State_Running.
HLE_NAMED("quartz.dll", "IMediaControl::GetState", Control_GetState, 3) {
  Graph& g = *Self(c, "IMediaControl")->graph;
  if (!Arg(c, 2)) return E_POINTER;
  Wr32(Arg(c, 2), uint32_t(g.player.state()));
  return S_OK;
}

audio::MediaPlayer& Media(Ctx& c) {
  Graph& g = *Self(c, "IMediaSeeking")->graph;
  if (!g.player.loaded()) FatalF("IMediaSeeking call on a graph without media");
  return g.player;
}

HLE_NAMED("quartz.dll", "IMediaSeeking::GetCapabilities", Seeking_GetCapabilities, 2) {
  Media(c);
  if (!Arg(c, 1)) return E_POINTER;
  Wr32(Arg(c, 1), 0x3F);
  return S_OK;
}
HLE_NAMED("quartz.dll", "IMediaSeeking::GetTimeFormat", Seeking_GetTimeFormat, 2) {
  Media(c);
  if (!Arg(c, 1)) return E_POINTER;
  uint8_t b[16];
  std::memcpy(b, &kTIME_FORMAT_MEDIA_TIME, 16);
  for (int i = 0; i < 16; i++) Wr8(Arg(c, 1) + i, b[i]);
  return S_OK;
}
HLE_NAMED("quartz.dll", "IMediaSeeking::SetTimeFormat", Seeking_SetTimeFormat, 2) {
  Media(c);
  return Arg(c, 1) && com::GuidEq(Arg(c, 1), kTIME_FORMAT_MEDIA_TIME) ? S_OK : E_INVALIDARG;
}
HLE_NAMED("quartz.dll", "IMediaSeeking::GetDuration", Seeking_GetDuration, 2) {
  audio::MediaPlayer& g = Media(c);
  if (!Arg(c, 1)) return E_POINTER;
  Wr64(Arg(c, 1), uint64_t(g.Duration()));
  return S_OK;
}
HLE_NAMED("quartz.dll", "IMediaSeeking::GetStopPosition", Seeking_GetStopPosition, 2) {
  audio::MediaPlayer& g = Media(c);
  if (!Arg(c, 1)) return E_POINTER;
  Wr64(Arg(c, 1), uint64_t(g.StopPosition()));
  return S_OK;
}
HLE_NAMED("quartz.dll", "IMediaSeeking::GetCurrentPosition", Seeking_GetCurrentPosition, 2) {
  audio::MediaPlayer& g = Media(c);
  if (!Arg(c, 1)) return E_POINTER;
  Wr64(Arg(c, 1), uint64_t(g.Position()));
  return S_OK;
}
// (pCurrent, pStop)
HLE_NAMED("quartz.dll", "IMediaSeeking::GetPositions", Seeking_GetPositions, 3) {
  audio::MediaPlayer& g = Media(c);
  if (Arg(c, 1)) Wr64(Arg(c, 1), uint64_t(g.Position()));
  if (Arg(c, 2)) Wr64(Arg(c, 2), uint64_t(g.StopPosition()));
  return S_OK;
}
// (pEarliest, pLatest)
HLE_NAMED("quartz.dll", "IMediaSeeking::GetAvailable", Seeking_GetAvailable, 3) {
  audio::MediaPlayer& g = Media(c);
  if (Arg(c, 1)) Wr64(Arg(c, 1), 0);
  if (Arg(c, 2)) Wr64(Arg(c, 2), uint64_t(g.Duration()));
  return S_OK;
}
// (pCurrent, dwCurrentFlags, pStop, dwStopFlags): the low bits of a flag
// word are AM_SEEKING_PositioningBitMask, bit 3 is ReturnTime: the
// positions reached are written back.
HLE_NAMED("quartz.dll", "IMediaSeeking::SetPositions", Seeking_SetPositions, 5) {
  audio::MediaPlayer& g = Media(c);
  uint32_t cm = Arg(c, 2) & 7, sm = Arg(c, 4) & 7;
  if ((cm && !Arg(c, 1)) || (sm && !Arg(c, 3))) return E_POINTER;
  int64_t cur = cm ? int64_t(Rd64(Arg(c, 1))) : 0, stop = sm ? int64_t(Rd64(Arg(c, 3))) : 0;
  g.SetPositions(cm, &cur, sm, &stop);
  if ((Arg(c, 2) & 8) && Arg(c, 1)) Wr64(Arg(c, 1), uint64_t(cur));
  if ((Arg(c, 4) & 8) && Arg(c, 3)) Wr64(Arg(c, 3), uint64_t(stop));
  return S_OK;
}
// SetRate takes a double by value: two argument slots.
HLE_NAMED("quartz.dll", "IMediaSeeking::SetRate", Seeking_SetRate, 3) {
  Media(c);
  uint64_t bits = uint64_t(Arg(c, 1)) | uint64_t(Arg(c, 2)) << 32;
  double r;
  std::memcpy(&r, &bits, 8);
  return r == 1.0 ? S_OK : E_INVALIDARG;
}
HLE_NAMED("quartz.dll", "IMediaSeeking::GetRate", Seeking_GetRate, 2) {
  Media(c);
  if (!Arg(c, 1)) return E_POINTER;
  double r = 1.0;
  uint64_t bits;
  std::memcpy(&bits, &r, 8);
  Wr64(Arg(c, 1), bits);
  return S_OK;
}

HLE_NAMED("quartz.dll", "IBasicAudio::put_Volume", Audio_put_Volume, 2) {
  return Self(c, "IBasicAudio")->graph->player.SetVolume(int32_t(Arg(c, 1))) ? S_OK : E_INVALIDARG;
}
HLE_NAMED("quartz.dll", "IBasicAudio::get_Volume", Audio_get_Volume, 2) {
  Iface* self = Self(c, "IBasicAudio");
  if (!Arg(c, 1)) return E_POINTER;
  Wr32(Arg(c, 1), uint32_t(self->graph->player.volume()));
  return S_OK;
}

}  // namespace
}  // namespace recomp::hle
