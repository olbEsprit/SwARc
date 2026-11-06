#pragma once
// The fixed-function vertex stage as a generated vertex shader, for draws
// that pair it with a pixel shader.

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "runtime/gfx/device.h"
#include "runtime/hle/d3d9_types.h"
#include "runtime/hlsl/hlsl.h"

namespace recomp::d3d9 {

struct FixedVertexState {
  const std::vector<guest::D3DVERTEXELEMENT9>* elements;  // the vertex declaration
  const std::vector<hlsl::Semantic>* ps_inputs;           // varyings the pixel shader reads
  const uint32_t (*tss)[33];                               // texture stage states, 8 stages
  bool lighting;                                           // D3DRS_LIGHTING
};

// HLSL (vs_3_0, entry "main") writing exactly the pixel shader's inputs;
// its only constant is `float4x4 wvp` (c0-c3, row-major). Fatal for what
// the generated stage does not do yet.
std::string FixedVertexSource(const FixedVertexState& s);

// The declaration's element for each vertex shader input slot (a slot the
// declaration lacks gets the default input).
std::vector<gfx::VertexInput> VertexInputs(const std::vector<hlsl::Semantic>& slots,
                                           const std::vector<guest::D3DVERTEXELEMENT9>& elements);

// WORLD * VIEW * PROJECTION as D3D stores them (row vectors); a transform
// never set is the identity.
void WorldViewProjection(const std::map<uint32_t, std::array<float, 16>>& transforms,
                         float out[16]);

}  // namespace recomp::d3d9
