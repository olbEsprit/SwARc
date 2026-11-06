// Direct3D 9 draws. The device's state reaches gfx::Device at each draw
// (render states that changed, viewport, scissor, targets). Draws with
// shaders (translated from D3DXCompileShader's HLSL or from bytecode)
// become gfx::ShaderDraw: the vertex declaration (or FVF) is matched to the
// vertex shader's input semantics, and the streams, indices, constant
// registers and samplers are passed as they are. Draws without shaders
// use gfx::Device's fixed-function pipeline (D3D7 semantics) with the
// D3D9 state in its terms: sampler states at D3D7's texture-stage
// positions, D3D9's transform numbers mapped. A vertex shader with no pixel
// shader gets a pixel shader generated from the texture-stage state, and a
// pixel shader with no vertex shader a vertex shader generated from the
// vertex declaration (d3d9_fixed_vertex.cpp); fixed-function draws with a
// vertex declaration (no FVF) are not supported yet (fatal, named).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "runtime/hle/d3d9_fixed_vertex.h"
#include "runtime/hle/d3d9_impl.h"
#include "runtime/hle/hle.h"
#include "runtime/log.h"

namespace recomp::d3d9 {

void SyncState(Device& d) {
  for (uint32_t s = 0; s < 256; s++) {
    if (d.synced && d.sent_rs[s] == d.rs[s]) continue;
    d.gfx->SetRenderState(s, d.rs[s]);
    d.sent_rs[s] = d.rs[s];
  }
  d.synced = true;
  const guest::D3DVIEWPORT9& v = d.viewport;
  d.gfx->SetViewport({v.X, v.Y, v.Width, v.Height, v.MinZ, v.MaxZ});
  const guest::D3DRECT& r = d.scissor;
  d.gfx->SetScissor({r.x1, r.y1, r.x2, r.y2});
}

namespace {

// D3DDECLTYPE, D3DDECLUSAGE
enum : uint8_t {
  FLOAT1 = 0, FLOAT2 = 1, FLOAT3 = 2, FLOAT4 = 3, D3DCOLOR = 4, UBYTE4 = 5, UNUSED = 17,
};
enum : uint8_t {
  USAGE_POSITION = 0, USAGE_BLENDWEIGHT = 1, USAGE_BLENDINDICES = 2, USAGE_NORMAL = 3,
  USAGE_PSIZE = 4, USAGE_TEXCOORD = 5, USAGE_POSITIONT = 9, USAGE_COLOR = 10,
};

// The vertex declaration D3D9 makes for an FVF.
std::vector<guest::D3DVERTEXELEMENT9> FvfElements(uint32_t fvf) {
  std::vector<guest::D3DVERTEXELEMENT9> e;
  uint16_t off = 0;
  auto add = [&](uint8_t type, uint8_t usage, uint8_t index, uint16_t size) {
    e.push_back({0, off, type, 0, usage, index});
    off = uint16_t(off + size);
  };
  uint32_t pos = fvf & 0x400E;
  if (pos == 0x004) {  // XYZRHW
    add(FLOAT4, USAGE_POSITIONT, 0, 16);
  } else if (pos == 0x4002) {  // XYZW
    add(FLOAT4, USAGE_POSITION, 0, 16);
  } else if (pos >= 0x002 && pos <= 0x00E) {
    add(FLOAT3, USAGE_POSITION, 0, 12);
    // XYZB1-5: blend weights; with LASTBETA_UBYTE4 / _D3DCOLOR the last
    // one is the blend indices.
    int betas = pos == 0x002 ? 0 : int((pos - 4) / 2);
    bool indices = fvf & (0x1000 | 0x8000);
    int weights = betas - (indices ? 1 : 0);
    if (weights > 0) add(uint8_t(FLOAT1 + weights - 1), USAGE_BLENDWEIGHT, 0, uint16_t(4 * weights));
    if (indices) add((fvf & 0x8000) ? D3DCOLOR : UBYTE4, USAGE_BLENDINDICES, 0, 4);
  }
  if (fvf & 0x010) add(FLOAT3, USAGE_NORMAL, 0, 12);
  if (fvf & 0x020) add(FLOAT1, USAGE_PSIZE, 0, 4);
  if (fvf & 0x040) add(D3DCOLOR, USAGE_COLOR, 0, 4);
  if (fvf & 0x080) add(D3DCOLOR, USAGE_COLOR, 1, 4);
  uint32_t ntex = (fvf >> 8) & 0xF;
  for (uint32_t i = 0; i < ntex && i < 8; i++) {
    static const uint8_t kTypes[4] = {FLOAT2, FLOAT3, FLOAT4, FLOAT1};
    uint8_t t = kTypes[(fvf >> (16 + 2 * i)) & 3];
    add(t, USAGE_TEXCOORD, uint8_t(i), uint16_t(4 * (t - FLOAT1 + 1)));
  }
  e.push_back({0xFF, 0, UNUSED, 0, 0, 0});  // D3DDECL_END
  return e;
}

// Vertices (or indices) a draw of `count` primitives uses.
uint32_t VerticesFor(uint32_t prim, uint32_t count) {
  switch (prim) {
    case 1: return count;          // POINTLIST
    case 2: return count * 2;      // LINELIST
    case 3: return count + 1;      // LINESTRIP
    case 4: return count * 3;      // TRIANGLELIST
    case 5: case 6: return count + 2;  // TRIANGLESTRIP, TRIANGLEFAN
    default: return 0;
  }
}

// The fixed-function state in gfx::Device's (D3D7's) terms.
void SyncFixedFunction(Device& d) {
  // Stages from the first with COLOROP DISABLE (1) on are unused.
  bool active = true;
  for (uint32_t s = 0; s < 8; s++) {
    active = active && d.tss[s][1] != 1;
    for (uint32_t t = 1; t < 33; t++)
      if (t < 13 || t > 21) d.gfx->SetTextureStageState(s, t, d.tss[s][t]);
    // D3D9 moved these to sampler states. Filters: D3DTEXF (NONE 0, POINT
    // 1, LINEAR 2, ANISOTROPIC 3) to D3D7's magnification (anisotropic 5)
    // and mip (NONE 1, POINT 2, LINEAR 3) values.
    const uint32_t* ss = d.ss[s];
    d.gfx->SetTextureStageState(s, 13, ss[1]);  // ADDRESSU
    d.gfx->SetTextureStageState(s, 14, ss[2]);  // ADDRESSV
    d.gfx->SetTextureStageState(s, 15, ss[4]);  // BORDERCOLOR
    d.gfx->SetTextureStageState(s, 16, ss[5] == 3 ? 5 : ss[5]);  // MAGFILTER
    d.gfx->SetTextureStageState(s, 17, ss[6]);  // MINFILTER
    d.gfx->SetTextureStageState(s, 18, ss[7] + 1);  // MIPFILTER
    d.gfx->SetTextureStageState(s, 19, ss[8]);  // MIPMAPLODBIAS
    d.gfx->SetTextureStageState(s, 20, ss[9]);  // MAXMIPLEVEL
    d.gfx->SetTextureStageState(s, 21, ss[10]);  // MAXANISOTROPY
    auto* t = active ? com::As<Texture>(d.textures[s]) : nullptr;
    d.gfx->SetTexture(s, t ? t->tex : 0);
  }
  // D3DTS_VIEW 2, PROJECTION 3 and TEXTURE0-7 16-23 are D3D7's numbers;
  // WORLD-WORLD3 are 256-259 (D3D7: 1, 4-6).
  static const std::pair<uint32_t, uint32_t> kTransforms[] = {
      {2, 2}, {3, 3}, {256, 1}, {257, 4}, {258, 5}, {259, 6}, {16, 16}, {17, 17}, {18, 18},
      {19, 19}, {20, 20}, {21, 21}, {22, 22}, {23, 23},
  };
  static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  for (const auto& [d3d9, d3d7] : kTransforms) {
    auto it = d.transforms.find(d3d9);
    d.gfx->SetTransform(d3d7, it != d.transforms.end() ? it->second.data() : kIdentity);
  }
  gfx::Material gm{};
  static_assert(sizeof gm == sizeof d.material);
  std::memcpy(&gm, &d.material, sizeof gm);
  d.gfx->SetMaterial(gm);
  for (const auto& [index, light] : d.lights) {
    gfx::Light gl;
    static_assert(sizeof gl == sizeof light);
    std::memcpy(&gl, &light, sizeof gl);
    d.gfx->SetLight(index, gl);
    auto it = d.light_on.find(index);
    d.gfx->LightEnable(index, it != d.light_on.end() && it->second);
  }
  for (uint32_t i = 0; i < 6; i++) d.gfx->SetClipPlane(i, d.clip_planes[i]);
}

// A draw without shaders: stream 0 with the FVF.
void DrawFixed(Device& d, gfx::ShaderDraw& sd, const char* what) {
  if (com::As<VertexDecl>(d.decl) || !d.fvf)
    FatalF("d3d9: fixed-function %s with a vertex declaration (only FVFs are supported)", what);
  if (!sd.streams[0].data) {
    const Device::Stream& st = d.streams[0];
    auto* b = com::As<Buffer>(st.buffer);
    if (!b || st.offset >= b->length) return;
    sd.streams[0] = {static_cast<const uint8_t*>(GPtr(b->mem)) + st.offset, b->length - st.offset,
                     st.stride};
  }
  const gfx::VertexStream& vs = sd.streams[0];
  uint32_t first = uint32_t(std::max(sd.base_vertex, 0));
  // Vertices from base_vertex; indexed draws index from there.
  uint32_t nv = sd.indices ? sd.min_index + sd.vertex_count : sd.vertex_count;
  if (!vs.stride || uint64_t(first + nv) * vs.stride > vs.size)
    FatalF("d3d9: fixed-function %s reads past its vertex buffer", what);
  std::vector<uint16_t> idx;
  uint32_t ni = 0;
  if (sd.indices) {
    ni = VerticesFor(sd.prim, sd.primitives);
    idx.resize(ni);
    for (uint32_t i = 0; i < ni; i++) {
      uint32_t v = sd.index_size == 4 ? static_cast<const uint32_t*>(sd.indices)[i]
                                      : static_cast<const uint16_t*>(sd.indices)[i];
      if (v > 0xFFFF) FatalF("d3d9: fixed-function %s with an index above 65535", what);
      idx[i] = uint16_t(v);
    }
  }
  SyncState(d);
  SyncFixedFunction(d);
  BindTargets(d);
  d.gfx->Draw(sd.prim, d.fvf, vs.data + size_t(first) * vs.stride, nv, vs.stride,
              sd.indices ? idx.data() : nullptr, ni);
}

// The pixel shader for a draw with `vs` and no pixel shader: the texture-
// stage cascade as straight-line code (ff.frag's Arg and Op maths), reading
// only the varyings `vs` writes.
std::string FixedFunctionPixelSource(const Device& d, const hlsl::Program& vs) {
  auto has = [&](uint32_t usage, uint32_t index) {
    return std::find(vs.varyings.begin(), vs.varyings.end(), hlsl::Semantic{usage, index}) !=
           vs.varyings.end();
  };
  bool uses_spec = false, uses_tfactor = false, uses_splat = false;
  std::set<uint32_t> texcoords;
  auto arg = [&](uint32_t a) {
    std::string v;
    switch (a & 0xF) {
      case 0: v = "diff"; break;
      case 1: v = "cur"; break;
      case 2: v = "tex"; break;
      case 3: uses_tfactor = true; v = "tfactor"; break;
      case 4: uses_spec = true; v = "spec"; break;
      case 5: v = "temp"; break;
      default: v = "float4(0.0, 0.0, 0.0, 0.0)"; break;
    }
    if (a & 0x20) v = "float4(" + v + ".a, " + v + ".a, " + v + ".a, " + v + ".a)";
    if (a & 0x10) v = "1.0 - (" + v + ")";
    return v;
  };
  auto mask = [](uint32_t op) {
    if (op == 2) return 1;
    if (op == 3) return 2;
    if (op == 17 || op == 22 || op == 23) return 1;
    if (op == 25 || op == 26) return 7;
    return 3;
  };
  auto op = [&](uint32_t o, const std::string& a1, const std::string& a2,
                const std::string& a0) {
    switch (o) {
      case 2: return a1;
      case 3: return a2;
      case 4: return a1 + " * " + a2;
      case 5: return a1 + " * " + a2 + " * 2.0";
      case 6: return a1 + " * " + a2 + " * 4.0";
      case 7: return a1 + " + " + a2;
      case 8: return a1 + " + " + a2 + " - 0.5";
      case 9: return "(" + a1 + " + " + a2 + " - 0.5) * 2.0";
      case 10: return a1 + " - " + a2;
      case 11: return a1 + " + " + a2 + " - " + a1 + " * " + a2;
      case 12: return "lerp(" + a2 + ", " + a1 + ", diff.a)";
      case 13: return "lerp(" + a2 + ", " + a1 + ", tex.a)";
      case 14: uses_tfactor = true; return "lerp(" + a2 + ", " + a1 + ", tfactor.a)";
      case 15: return a1 + " + " + a2 + " * (1.0 - tex.a)";
      case 16: return "lerp(" + a2 + ", " + a1 + ", cur.a)";
      case 17: return a1;
      case 18: return "float4(" + a1 + ".rgb + " + a1 + ".a * " + a2 + ".rgb, " + a1 + ".a)";
      case 19: return "float4(" + a1 + ".rgb * " + a2 + ".rgb + " + a1 + ".a, " + a1 + ".a)";
      case 20:
        return "float4((1.0 - " + a1 + ".a) * " + a2 + ".rgb + " + a1 + ".rgb, " + a1 + ".a)";
      case 21:
        return "float4((1.0 - " + a1 + ".rgb) * " + a2 + ".rgb + " + a1 + ".a, " + a1 + ".a)";
      case 24:
        uses_splat = true;
        return "ff_splat(saturate(4.0 * dot(" + a1 + ".rgb - 0.5, " + a2 + ".rgb - 0.5)))";
      case 25: return a0 + " + " + a1 + " * " + a2;
      case 26: return "lerp(" + a2 + ", " + a1 + ", " + a0 + ")";
      default: return a1;
    }
  };
  auto sample = [&](uint32_t s, const Texture& t) {
    uint32_t k = d.tss[s][11] & 0xFFFF;
    std::string coord = "float4(0.0, 0.0, 0.0, 0.0)";
    if (has(hlsl::kTexcoord, k)) {
      coord = "v_texcoord" + std::to_string(k);
      texcoords.insert(k);
    }
    bool proj = d.tss[s][24] & 0x100;
    std::string sn = "s" + std::to_string(s);
    if (t.cube)
      return proj ? "texCUBEproj(" + sn + ", " + coord + ")"
                  : "texCUBE(" + sn + ", (" + coord + ").xyz)";
    return proj ? "tex2Dproj(" + sn + ", " + coord + ")"
                : "tex2D(" + sn + ", (" + coord + ").xy)";
  };

  std::string samplers, body;
  for (uint32_t s = 0; s < 8; s++) {
    uint32_t cop = d.tss[s][1];
    if (cop == 1) break;  // DISABLE ends the cascade
    body += "  {\n";
    auto* t = com::As<Texture>(d.textures[s]);
    if (t) {
      samplers += std::string(t->cube ? "samplerCUBE " : "sampler2D ") + "s" +
                  std::to_string(s) + " : register(s" + std::to_string(s) + ");\n";
      body += "    float4 tex = " + sample(s, *t) + ";\n";
    } else {
      body += "    float4 tex = float4(0.0, 0.0, 0.0, 1.0);\n";
    }
    uint32_t m = mask(cop);
    if (m & 1) body += "    float4 c1 = " + arg(d.tss[s][2]) + ";\n";
    if (m & 2) body += "    float4 c2 = " + arg(d.tss[s][3]) + ";\n";
    if (m & 4) body += "    float4 c0 = " + arg(d.tss[s][26]) + ";\n";
    body += "    float3 rgb = saturate((" + op(cop, "c1", "c2", "c0") + ").rgb);\n";
    body += "    float al = cur.a;\n";
    uint32_t aop = d.tss[s][4];
    if (aop != 1) {
      uint32_t am = mask(aop);
      if (am & 1) body += "    float4 d1 = " + arg(d.tss[s][5]) + ";\n";
      if (am & 2) body += "    float4 d2 = " + arg(d.tss[s][6]) + ";\n";
      if (am & 4) body += "    float4 d0 = " + arg(d.tss[s][27]) + ";\n";
      body += "    al = saturate((" + op(aop, "d1", "d2", "d0") + ").a);\n";
    }
    if (cop == 24) body += "    al = rgb.r;\n";  // DOTPRODUCT3 replicates into alpha
    if (d.tss[s][28] == 5)
      body += "    temp = float4(rgb, al);\n";
    else
      body += "    cur = float4(rgb, al);\n";
    body += "  }\n";
  }
  if (d.rs[29]) {
    uses_spec = true;
    body += "  cur.rgb = saturate(cur.rgb + spec.rgb);\n";
  }

  bool has_diff = has(hlsl::kColor, 0), has_spec = has(hlsl::kColor, 1);
  std::string params;
  auto param = [&](const std::string& name, const std::string& sem) {
    if (!params.empty()) params += ", ";
    params += "float4 " + name + " : " + sem;
  };
  if (has_diff) param("v_color0", "COLOR0");
  if (uses_spec && has_spec) param("v_color1", "COLOR1");
  for (uint32_t k : texcoords) {
    param("v_texcoord" + std::to_string(k), "TEXCOORD" + std::to_string(k));
  }

  std::string src = samplers.empty() ? "" : samplers + "\n";
  if (uses_tfactor) src += "float4 tfactor : register(c0);\n\n";
  if (uses_splat) src += "float4 ff_splat(float x) { return float4(x, x, x, x); }\n\n";
  src += "float4 main(" + params + ") : COLOR0 {\n";
  src += has_diff ? "  float4 diff = v_color0;\n"
                  : "  float4 diff = float4(1.0, 1.0, 1.0, 1.0);\n";
  if (uses_spec)
    src += has_spec ? "  float4 spec = v_color1;\n"
                    : "  float4 spec = float4(0.0, 0.0, 0.0, 0.0);\n";
  src += "  float4 cur = diff;\n  float4 temp = float4(0.0, 0.0, 0.0, 0.0);\n";
  src += body + "  return cur;\n}\n";
  return src;
}

// A generated shader for `source`, created on first use and kept in the
// device (each source is one gfx shader).
Shader* Generated(Device& d, std::map<std::string, std::shared_ptr<Shader>>& cache,
                  const std::string& source, bool pixel) {
  auto it = cache.find(source);
  if (it != cache.end()) return it->second.get();
  auto program = std::make_shared<hlsl::Program>();
  std::string errors;
  if (!hlsl::Compile(source, {}, "main", pixel ? "ps_3_0" : "vs_3_0", 0, program.get(), &errors))
    FatalF("d3d9: the generated fixed-function %s shader does not compile:\n%s\n%s",
           pixel ? "pixel" : "vertex", errors.c_str(), source.c_str());
  auto s = std::make_shared<Shader>();
  s->device = d.guest;
  s->pixel = pixel;
  s->program = std::move(program);
  s->id = d.gfx->CreateShader(*s->program);
  return cache.emplace(source, std::move(s)).first->second.get();
}

// Completes `sd` (primitive, counts, and any streams already given) from
// the device's bindings and draws it.
void Draw(Device& d, gfx::ShaderDraw& sd, const char* what) {
  Shader* vs = com::As<Shader>(d.vs);
  Shader* ps = com::As<Shader>(d.ps);
  if (!vs && !ps) {
    DrawFixed(d, sd, what);
    return;
  }
  bool ff_ps = !ps;
  std::vector<guest::D3DVERTEXELEMENT9> from_fvf;
  const std::vector<guest::D3DVERTEXELEMENT9>* elements = nullptr;
  if (auto* decl = com::As<VertexDecl>(d.decl)) {
    elements = &decl->elements;
  } else if (d.fvf) {
    from_fvf = FvfElements(d.fvf);
    elements = &from_fvf;
  } else {
    FatalF("d3d9: %s without a vertex declaration or FVF", what);
  }
  float ff_constants[4][4];
  const float(*vs_float)[4] = d.vs_f;
  if (!vs) {
    FixedVertexState fv{elements, &ps->program->varyings, d.tss, d.rs[137] != 0};
    vs = Generated(d, d.ff_vertex, FixedVertexSource(fv), false);
    WorldViewProjection(d.transforms, &ff_constants[0][0]);
    vs_float = ff_constants;
  } else if (!ps) {
    ps = Generated(d, d.ff_pixel, FixedFunctionPixelSource(d, *vs->program), true);
  }
  std::vector<gfx::VertexInput> inputs = VertexInputs(vs->program->inputs, *elements);
  sd.inputs = inputs.data();
  sd.input_count = uint32_t(inputs.size());

  for (int s = 0; s < 16; s++) {
    const Device::Stream& st = d.streams[s];
    if (st.freq != 1 && st.buffer)
      FatalF("d3d9: %s with stream %d frequency %08X (instancing is not supported yet)", what, s,
             st.freq);
    if (sd.streams[s].data) continue;  // given by the caller (UP draws)
    auto* b = com::As<Buffer>(st.buffer);
    if (!b || st.offset >= b->length) continue;
    sd.streams[s] = {static_cast<const uint8_t*>(GPtr(b->mem)) + st.offset, b->length - st.offset,
                     st.stride};
  }

  sd.vs = vs->id;
  sd.ps = ps->id;
  sd.vs_float = vs_float;
  sd.vs_int = d.vs_i;
  sd.vs_bool = reinterpret_cast<const uint32_t*>(d.vs_b);
  sd.ps_float = d.ps_f;
  float ff_f[224][4];
  if (ff_ps && ps->program->float_regs) {
    std::memcpy(ff_f, d.ps_f, sizeof ff_f);
    uint32_t tf = d.rs[60];
    ff_f[0][0] = float(tf >> 16 & 0xFF) / 255.0f;
    ff_f[0][1] = float(tf >> 8 & 0xFF) / 255.0f;
    ff_f[0][2] = float(tf & 0xFF) / 255.0f;
    ff_f[0][3] = float(tf >> 24 & 0xFF) / 255.0f;
    sd.ps_float = ff_f;
  }
  sd.ps_int = d.ps_i;
  sd.ps_bool = reinterpret_cast<const uint32_t*>(d.ps_b);
  for (int i = 0; i < 20; i++) {
    auto* t = com::As<Texture>(d.textures[i]);
    sd.textures[i] = t ? t->tex : 0;
  }
  sd.sampler_states = d.ss;
  SyncState(d);
  BindTargets(d);
  d.gfx->DrawShader(sd);
}

}  // namespace
}  // namespace recomp::d3d9

namespace recomp::hle {
namespace {

using namespace d3d9;

Device* Dev(Ctx& c) { return DeviceOf(Arg(c, 0)); }

// (PrimitiveType, StartVertex, PrimitiveCount)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::DrawPrimitive", D3DDev9_DrawPrimitive, 4) {
  gfx::ShaderDraw sd;
  sd.prim = Arg(c, 1);
  sd.primitives = Arg(c, 3);
  sd.base_vertex = int32_t(Arg(c, 2));
  sd.vertex_count = VerticesFor(sd.prim, sd.primitives);
  if (!sd.vertex_count) return D3DERR_INVALIDCALL;
  Draw(*Dev(c), sd, "DrawPrimitive");
  return D3D_OK;
}

// (Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::DrawIndexedPrimitive", D3DDev9_DrawIndexedPrimitive, 7) {
  Device* d = Dev(c);
  auto* ib = com::As<Buffer>(d->indices);
  gfx::ShaderDraw sd;
  sd.prim = Arg(c, 1);
  sd.primitives = Arg(c, 6);
  uint32_t n = VerticesFor(sd.prim, sd.primitives);
  if (!ib || !n) return D3DERR_INVALIDCALL;
  sd.index_size = ib->fvf_or_format == D3DFMT_INDEX32 ? 4 : 2;
  uint64_t start = uint64_t(Arg(c, 5)) * sd.index_size;
  if (start + uint64_t(n) * sd.index_size > ib->length) return D3DERR_INVALIDCALL;
  sd.indices = static_cast<const uint8_t*>(GPtr(ib->mem)) + start;
  sd.base_vertex = int32_t(Arg(c, 2));
  sd.min_index = Arg(c, 3);
  sd.vertex_count = Arg(c, 4);
  Draw(*d, sd, "DrawIndexedPrimitive");
  return D3D_OK;
}

// (PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride).
// Stream 0 is unbound afterwards, as in D3D9.
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::DrawPrimitiveUP", D3DDev9_DrawPrimitiveUP, 5) {
  Device* d = Dev(c);
  gfx::ShaderDraw sd;
  sd.prim = Arg(c, 1);
  sd.primitives = Arg(c, 2);
  uint32_t stride = Arg(c, 4);
  sd.vertex_count = VerticesFor(sd.prim, sd.primitives);
  uint32_t bytes = sd.vertex_count * stride;
  if (!sd.vertex_count || !EnsureReadable(c, Arg(c, 3), bytes)) return D3DERR_INVALIDCALL;
  sd.streams[0] = {static_cast<const uint8_t*>(GPtr(Arg(c, 3))), bytes, stride};
  Draw(*d, sd, "DrawPrimitiveUP");
  Bind(&d->streams[0].buffer, 0);
  d->streams[0].offset = d->streams[0].stride = 0;
  return D3D_OK;
}

// (PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData,
// IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride). Stream 0
// and the indices are unbound afterwards, as in D3D9.
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::DrawIndexedPrimitiveUP", D3DDev9_DrawIndexedPrimitiveUP, 9) {
  Device* d = Dev(c);
  gfx::ShaderDraw sd;
  sd.prim = Arg(c, 1);
  sd.min_index = Arg(c, 2);
  sd.vertex_count = Arg(c, 3);
  sd.primitives = Arg(c, 4);
  sd.index_size = Arg(c, 6) == D3DFMT_INDEX32 ? 4 : 2;
  uint32_t n = VerticesFor(sd.prim, sd.primitives), stride = Arg(c, 8);
  uint32_t vbytes = (sd.min_index + sd.vertex_count) * stride;
  if (!n || !EnsureReadable(c, Arg(c, 5), n * sd.index_size) ||
      !EnsureReadable(c, Arg(c, 7), vbytes))
    return D3DERR_INVALIDCALL;
  sd.indices = GPtr(Arg(c, 5));
  sd.streams[0] = {static_cast<const uint8_t*>(GPtr(Arg(c, 7))), vbytes, stride};
  Draw(*d, sd, "DrawIndexedPrimitiveUP");
  Bind(&d->streams[0].buffer, 0);
  d->streams[0].offset = d->streams[0].stride = 0;
  Bind(&d->indices, 0);
  return D3D_OK;
}

// The gfx colour target behind a surface for StretchRect (tex 0: the back
// buffer); fatal for the kinds it does not support.
gfx::ColorTarget StretchTarget(Surface* s, const char* which) {
  switch (s->kind) {
    case Surface::Kind::BackBuffer:
      return {};
    case Surface::Kind::RenderTarget:
      return {s->tex, 0};
    case Surface::Kind::TextureLevel:
      if (s->texture->usage & D3DUSAGE_RENDERTARGET)
        return {s->texture->tex, int(s->level), s->texture->cube ? s->face : 0};
      break;
    default:
      break;
  }
  FatalF("d3d9: StretchRect %s is not a colour target (kind %d, level %d)", which, int(s->kind),
         s->level);
}

gfx::Rect SurfaceRect(Surface* s, uint32_t rect) {
  if (!rect) return {0, 0, int32_t(s->desc.Width), int32_t(s->desc.Height)};
  const auto* r = G<guest::RECT>(rect);
  return {r->left, r->top, r->right, r->bottom};
}

// (pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::StretchRect", D3DDev9_StretchRect, 6) {
  Device* d = Dev(c);
  auto* src = com::As<Surface>(Arg(c, 1));
  auto* dst = com::As<Surface>(Arg(c, 3));
  if (!src || !dst || src == dst) return D3DERR_INVALIDCALL;
  gfx::ColorTarget s = StretchTarget(src, "source"), t = StretchTarget(dst, "destination");
  // D3DTEXF_LINEAR (2) filters; NONE and POINT take the nearest texel.
  d->gfx->StretchRect(s, SurfaceRect(src, Arg(c, 2)), t, SurfaceRect(dst, Arg(c, 4)),
                      Arg(c, 5) >= 2);
  return D3D_OK;
}

}  // namespace
}  // namespace recomp::hle
