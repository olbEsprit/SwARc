#include "win32/d3d9_fixed_vertex.h"

#include <cmath>

#include "runtime/log.h"

namespace recomp::d3d9 {

std::string FixedVertexSource(const FixedVertexState& s) {
  auto has = [&](uint32_t usage, uint32_t index) {
    for (const auto& e : *s.elements) {
      if (e.Stream == 0xFF) break;
      if (e.Usage == usage && e.UsageIndex == index) return true;
    }
    return false;
  };
  const bool rhw = has(hlsl::kPositionT, 0);
  if (!rhw && !has(hlsl::kPosition, 0)) FatalF("d3d9: a pixel shader without a vertex shader and no position in the vertex declaration");
  if (has(hlsl::kBlendWeight, 0) || has(hlsl::kBlendIndices, 0))
    FatalF("d3d9: vertex blending in the fixed-function vertex stage with a pixel shader is not supported yet");

  std::string in = rhw ? "float4 pos : POSITIONT" : "float4 pos : POSITION", out = "out float4 opos : POSITION";
  std::string body = rhw ? "  float w = pos.w == 0.0 ? 1.0 : 1.0 / pos.w;\n"
                           "  opos = float4(((pos.x - vp.x) / vp.z * 2.0 - 1.0) * w, (1.0 - (pos.y - vp.y) / vp.w * 2.0) * w, pos.z * w, w);\n"
                         : "  opos = mul(pos, wvp);\n";
  bool lit = false;
  for (const hlsl::Semantic& v : *s.ps_inputs) {
    std::string idx = std::to_string(v.index);
    if (v.usage == hlsl::kColor && v.index < 2) {
      std::string o = "o_c" + idx;
      out += ", out float4 " + o + " : COLOR" + idx;
      if (s.lighting && !rhw) {
        lit = true;
        body += "  " + o + (v.index == 0 ? " = lit_d;\n" : " = lit_s;\n");
      } else if (has(hlsl::kColor, v.index)) {
        in += ", float4 c" + idx + " : COLOR" + idx;
        body += "  " + o + " = c" + idx + ";\n";
      } else {
        body += "  " + o + (v.index == 0 ? " = float4(1.0, 1.0, 1.0, 1.0);\n" : " = float4(0.0, 0.0, 0.0, 0.0);\n");
      }
    } else if (v.usage == hlsl::kTexcoord && v.index < 8) {
      uint32_t tci = s.tss[v.index][11], ttf = s.tss[v.index][24];
      if (tci >> 16) FatalF("d3d9: texture coordinate generation (stage %u, TEXCOORDINDEX %08X) in the fixed-function vertex stage with a pixel shader is not supported yet", v.index, tci);
      if (ttf & 0xFF) FatalF("d3d9: texture transform (stage %u, flags %08X) in the fixed-function vertex stage with a pixel shader is not supported yet", v.index, ttf);
      std::string o = "o_t" + idx, set = std::to_string(tci & 7);
      out += ", out float4 " + o + " : TEXCOORD" + idx;
      if (has(hlsl::kTexcoord, tci & 7)) {
        if (in.find("float4 t" + set + " :") == std::string::npos)
          in += ", float4 t" + set + " : TEXCOORD" + set;
        body += "  " + o + " = t" + set + ";\n";
      } else {
        body += "  " + o + " = float4(0.0, 0.0, 0.0, 1.0);\n";
      }
    } else {
      FatalF("d3d9: pixel shader input (usage %u, index %u) has no fixed-function vertex output", v.usage, v.index);
    }
  }
  std::string globals = rhw ? "float4 vp;\n" : "float4x4 wvp;\n";
  if (lit) {
    // D3D9's lighting as the full fixed-function stage does it (gfx/shaders/ff.vert), with the lights' kinds, the
    // material's sources and the switches written into the source; constants: FixedLightingConstants.
    const uint32_t* rs = s.rs;
    const bool cv = rs[141] != 0, spec_on = rs[29] != 0, local = rs[142] != 0, norm = rs[143] != 0;
    const bool has_d = has(hlsl::kColor, 0), has_s = has(hlsl::kColor, 1), has_n = has(hlsl::kNormal, 0);
    if (has_d && in.find("float4 c0 :") == std::string::npos) in += ", float4 c0 : COLOR0";
    if (has_s && in.find("float4 c1 :") == std::string::npos) in += ", float4 c1 : COLOR1";
    if (has_n) in += ", float3 nrm : NORMAL";
    globals += "float4x4 wv : register(c4);\nfloat3x3 nm : register(c8);\nfloat4 g_amb : register(c11);\n"
               "float4 m_d : register(c12);\nfloat4 m_a : register(c13);\nfloat4 m_s : register(c14);\n"
               "float4 m_e : register(c15);\nfloat4 m_p : register(c16);\n";
    auto source = [&](uint32_t src, const char* mat) -> std::string {
      if (cv && src == 1 && has_d) return "c0";
      if (cv && src == 2 && has_s) return "c1";
      return mat;
    };
    std::string l = "  float4 vpos = mul(pos, wv);\n  float3 P = vpos.xyz / vpos.w;\n";
    l += has_n ? std::string("  float3 N = mul(nrm, nm);\n") + (norm ? "  if (dot(N, N) > 0.0) N = normalize(N);\n" : "")
               : "  float3 N = float3(0.0, 0.0, 0.0);\n";
    l += "  float4 md = " + source(rs[145], "m_d") + ";\n  float4 ms = " + source(rs[146], "m_s") +
         ";\n  float4 ma = " + source(rs[147], "m_a") + ";\n  float4 me = " + source(rs[148], "m_e") + ";\n";
    l += local ? "  float3 V = normalize(-P);\n" : "  float3 V = float3(0.0, 0.0, -1.0);\n";
    l += "  float3 amb = g_amb.rgb;\n  float3 dsum = float3(0.0, 0.0, 0.0);\n  float3 ssum = float3(0.0, 0.0, 0.0);\n";
    l += "  float3 L;\n  float att;\n  float ndl;\n  float dist;\n  float3 dl;\n  float rho;\n  float den;\n";
    for (size_t i = 0; i < s.light_types.size(); i++) {
      const std::string n = "l" + std::to_string(i);
      const uint32_t r = 17 + 7 * uint32_t(i);
      const char* f[7] = {"_d", "_s", "_a", "_p", "_dir", "_att", "_spot"};
      for (int k = 0; k < 7; k++) globals += "float4 " + n + f[k] + " : register(c" + std::to_string(r + k) + ");\n";
      const uint32_t type = s.light_types[i];
      if (type == 3) {
        l += "  L = " + n + "_dir.xyz;\n  att = 1.0;\n";
      } else {
        l += "  dl = " + n + "_p.xyz - P;\n  dist = length(dl);\n  L = dist > 0.0 ? dl / dist : float3(0.0, 0.0, 1.0);\n"
             "  den = " + n + "_att.x + " + n + "_att.y * dist + " + n + "_att.z * dist * dist;\n"
             "  att = dist > " + n + "_p.w ? 0.0 : (den > 0.0 ? 1.0 / den : 1.0);\n";
        if (type == 2)
          l += "  rho = dot(L, " + n + "_dir.xyz);\n"
               "  if (rho <= " + n + "_spot.y) att = 0.0;\n"
               "  else if (rho < " + n + "_spot.x) att *= pow((rho - " + n + "_spot.y) / (" + n + "_spot.x - " + n +
               "_spot.y), " + n + "_dir.w);\n";
      }
      l += "  amb += " + n + "_a.rgb * att;\n  ndl = dot(N, L);\n  if (ndl > 0.0) {\n    dsum += " + n +
           "_d.rgb * ndl * att;\n";
      if (spec_on)
        l += "    float ndh" + std::to_string(i) + " = dot(N, normalize(L + V));\n    if (ndh" + std::to_string(i) +
             " > 0.0) ssum += " + n + "_s.rgb * pow(ndh" + std::to_string(i) + ", m_p.x) * att;\n";
      l += "  }\n";
    }
    l += "  float4 lit_d = float4(saturate(me.rgb + ma.rgb * amb + md.rgb * dsum), md.a);\n"
         "  float4 lit_s = float4(saturate(ms.rgb * ssum), ms.a);\n";
    body = l + body;
  }
  return std::string("#pragma pack_matrix(row_major)\n") + globals + "void main(" + in + ", " + out + ") {\n" + body + "}\n";
}

std::vector<gfx::VertexInput> VertexInputs(const std::vector<hlsl::Semantic>& slots,
                                           const std::vector<guest::D3DVERTEXELEMENT9>& elements) {
  std::vector<gfx::VertexInput> inputs(slots.size());
  for (size_t i = 0; i < slots.size(); i++) {
    for (const auto& e : elements) {
      if (e.Stream == 0xFF) break;
      if (e.Usage == slots[i].usage && e.UsageIndex == slots[i].index) {
        inputs[i] = {e.Stream, e.Offset, e.Type};
        break;
      }
    }
  }
  return inputs;
}

void WorldViewProjection(const std::map<uint32_t, std::array<float, 16>>& transforms, float out[16]) {
  auto get = [&](uint32_t id) {
    auto it = transforms.find(id);
    if (it != transforms.end()) return it->second;
    return std::array<float, 16>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  };
  auto mul = [](const std::array<float, 16>& a, const std::array<float, 16>& b) {
    std::array<float, 16> r{};
    for (int i = 0; i < 4; i++)
      for (int j = 0; j < 4; j++)
        for (int k = 0; k < 4; k++) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return r;
  };
  auto m = mul(mul(get(256), get(2)), get(3));
  for (int i = 0; i < 16; i++) out[i] = m[i];
}

void FixedLightingConstants(const std::map<uint32_t, std::array<float, 16>>& transforms, const uint32_t* rs,
                            const guest::D3DMATERIAL9& material, const std::vector<const guest::D3DLIGHT9*>& lights,
                            float (*c)[4]) {
  auto get = [&](uint32_t id) {
    auto it = transforms.find(id);
    if (it != transforms.end()) return it->second;
    return std::array<float, 16>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  };
  const std::array<float, 16> w = get(256), v = get(2);
  std::array<float, 16> wv{};
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      for (int k = 0; k < 4; k++) wv[i * 4 + j] += w[i * 4 + k] * v[k * 4 + j];
  for (int i = 0; i < 16; i++) c[4 + i / 4][i % 4] = wv[i];
  // Normals are row vectors too: n * inverse(WV3)^T, the rows of the inverse's transpose.
  const float* m = wv.data();
  const float a = m[0], b = m[1], cc = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], k = m[10];
  const float A = e * k - f * h, B = -(d * k - f * g), C = d * h - e * g;
  const float det = a * A + b * B + cc * C, inv = det != 0 ? 1.0f / det : 0.0f;
  const float adj[9] = {A, -(b * k - cc * h), b * f - cc * e, B, a * k - cc * g, -(a * f - cc * d), C,
                        -(a * h - b * g), a * e - b * d};  // inverse = adj * inv, row-major
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) c[8 + i][j] = adj[j * 3 + i] * inv;
    c[8 + i][3] = 0;
  }
  auto argb = [](uint32_t x, float* o) {
    o[0] = float(x >> 16 & 0xFF) / 255.0f, o[1] = float(x >> 8 & 0xFF) / 255.0f, o[2] = float(x & 0xFF) / 255.0f;
    o[3] = float(x >> 24) / 255.0f;
  };
  auto col = [](const guest::D3DCOLORVALUE& x, float* o) { o[0] = x.r, o[1] = x.g, o[2] = x.b, o[3] = x.a; };
  argb(rs[139], c[11]);
  col(material.Diffuse, c[12]);
  col(material.Ambient, c[13]);
  col(material.Specular, c[14]);
  col(material.Emissive, c[15]);
  c[16][0] = material.Power, c[16][1] = c[16][2] = c[16][3] = 0;
  for (size_t i = 0; i < lights.size(); i++) {
    const guest::D3DLIGHT9& l = *lights[i];
    float(*o)[4] = c + 17 + 7 * i;
    col(l.Diffuse, o[0]);
    col(l.Specular, o[1]);
    col(l.Ambient, o[2]);
    const float p[3] = {l.Position.x, l.Position.y, l.Position.z}, dir[3] = {l.Direction.x, l.Direction.y, l.Direction.z};
    for (int j = 0; j < 3; j++) {
      o[3][j] = p[0] * v[j] + p[1] * v[4 + j] + p[2] * v[8 + j] + v[12 + j];
      o[4][j] = -(dir[0] * v[j] + dir[1] * v[4 + j] + dir[2] * v[8 + j]);
    }
    const float len = std::sqrt(o[4][0] * o[4][0] + o[4][1] * o[4][1] + o[4][2] * o[4][2]);
    if (len > 0)
      for (int j = 0; j < 3; j++) o[4][j] /= len;
    o[3][3] = l.Range > 0 ? l.Range : 1e30f;
    o[4][3] = l.Falloff;
    o[5][0] = l.Attenuation0, o[5][1] = l.Attenuation1, o[5][2] = l.Attenuation2, o[5][3] = 0;
    o[6][0] = std::cos(l.Theta / 2), o[6][1] = std::cos(l.Phi / 2), o[6][2] = o[6][3] = 0;
  }
}

}  // namespace recomp::d3d9
