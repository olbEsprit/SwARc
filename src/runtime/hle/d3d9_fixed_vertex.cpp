#include "runtime/hle/d3d9_fixed_vertex.h"

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
  for (const hlsl::Semantic& v : *s.ps_inputs) {
    std::string idx = std::to_string(v.index);
    if (v.usage == hlsl::kColor && v.index < 2) {
      static bool warned = false;
      if (s.lighting && !warned) {
        warned = true;
        WarnF("d3d9: lighting in the fixed-function vertex stage with a pixel shader is not done yet: drawn unlit\n");
      }
      std::string o = "o_c" + idx;
      out += ", out float4 " + o + " : COLOR" + idx;
      if (has(hlsl::kColor, v.index)) {
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
  return std::string("#pragma pack_matrix(row_major)\n") + (rhw ? "float4 vp;" : "float4x4 wvp;") + "\nvoid main(" + in + ", " + out + ") {\n" + body + "}\n";
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

}  // namespace recomp::d3d9
