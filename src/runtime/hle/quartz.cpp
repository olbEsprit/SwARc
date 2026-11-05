// DirectShow (quartz) as far as a game that plays an audio file through
// the filter graph uses it: CLSID_FilterGraph with IGraphBuilder,
// IMediaControl, IMediaSeeking and IBasicAudio. There is no decoder behind
// it, so RenderFile fails and the graph never runs.

#include <array>
#include <memory>
#include <string>

#include "runtime/hle/com.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/win32.h"
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
                   VFW_E_CANNOT_RENDER = 0x80040217, VFW_E_NOT_FOUND = 0x80040216, VFW_E_NOT_CONNECTED = 0x80040209;

enum Kind { kBuilder, kControl, kSeeking, kAudio, kKinds };

struct Graph {
  std::array<uint32_t, kKinds> iface{};
  int32_t volume = 0;
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

// (lpcwstrFile, lpcwstrPlayList)
HLE_NAMED("quartz.dll", "IGraphBuilder::RenderFile", Builder_RenderFile, 3) {
  Self(c, "IGraphBuilder");
  WarnF("quartz: RenderFile(%s): no decoder\n",
        Arg(c, 1) ? win32::Narrow(GWStr(Arg(c, 1))).c_str() : "");
  return VFW_E_CANNOT_RENDER;
}

// (lpcwstrFileName, lpcwstrFilterName, ppFilter)
HLE_NAMED("quartz.dll", "IGraphBuilder::AddSourceFilter", Builder_AddSourceFilter, 4) {
  Self(c, "IGraphBuilder");
  uint32_t out = Arg(c, 3);
  if (!out) return E_POINTER;
  Wr32(out, 0);
  WarnF("quartz: AddSourceFilter(%s): no decoder\n",
        Arg(c, 1) ? win32::Narrow(GWStr(Arg(c, 1))).c_str() : "");
  return VFW_E_NOT_FOUND;
}

// A graph without a source filter has no media: it runs and stops, and
// refuses everything that needs a stream.
HLE_NAMED("quartz.dll", "IMediaControl::Run", Control_Run, 1) { Self(c, "IMediaControl"); return S_OK; }
HLE_NAMED("quartz.dll", "IMediaControl::Pause", Control_Pause, 1) { Self(c, "IMediaControl"); return S_OK; }
HLE_NAMED("quartz.dll", "IMediaControl::Stop", Control_Stop, 1) { Self(c, "IMediaControl"); return S_OK; }

// (msTimeout, pfs): State_Stopped.
HLE_NAMED("quartz.dll", "IMediaControl::GetState", Control_GetState, 3) {
  Self(c, "IMediaControl");
  if (!Arg(c, 2)) return E_POINTER;
  Wr32(Arg(c, 2), 0);
  return S_OK;
}

#define QUARTZ_NOT_CONNECTED(Method, Nargs)                                             \
  HLE_NAMED("quartz.dll", "IMediaSeeking::" #Method, Seeking_##Method, Nargs) {         \
    Self(c, "IMediaSeeking");                                                           \
    return VFW_E_NOT_CONNECTED;                                                         \
  }
QUARTZ_NOT_CONNECTED(SetTimeFormat, 2)
QUARTZ_NOT_CONNECTED(GetDuration, 2)
QUARTZ_NOT_CONNECTED(GetCurrentPosition, 2)
QUARTZ_NOT_CONNECTED(SetPositions, 5)
QUARTZ_NOT_CONNECTED(GetPositions, 3)
QUARTZ_NOT_CONNECTED(GetStopPosition, 2)
QUARTZ_NOT_CONNECTED(GetTimeFormat, 2)
QUARTZ_NOT_CONNECTED(GetCapabilities, 2)
QUARTZ_NOT_CONNECTED(GetAvailable, 3)
QUARTZ_NOT_CONNECTED(SetRate, 3)
QUARTZ_NOT_CONNECTED(GetRate, 2)

HLE_NAMED("quartz.dll", "IBasicAudio::put_Volume", Audio_put_Volume, 2) {
  Self(c, "IBasicAudio")->graph->volume = int32_t(Arg(c, 1));
  return S_OK;
}
HLE_NAMED("quartz.dll", "IBasicAudio::get_Volume", Audio_get_Volume, 2) {
  Iface* self = Self(c, "IBasicAudio");
  if (!Arg(c, 1)) return E_POINTER;
  Wr32(Arg(c, 1), uint32_t(self->graph->volume));
  return S_OK;
}

}  // namespace
}  // namespace recomp::hle
