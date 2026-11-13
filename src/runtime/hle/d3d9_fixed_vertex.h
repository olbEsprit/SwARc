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
  const uint32_t* rs = nullptr;                            // render states (for lighting)
  std::vector<uint32_t> light_types;                       // D3DLIGHTTYPE of each enabled light, at most 8
};

// The lighting's constants of the generated stage, from c4 on (FixedVertexSource with lighting):
// c4-c7 WORLD * VIEW, c8-c10 its normals' matrix, c11 AMBIENT, c12-c15 the material's diffuse, ambient, specular,
// emissive, c16.x its power, then 7 a light (view space): diffuse, specular, ambient, position (w range),
// direction toward the light (w falloff), attenuation 0-2, the spot's cos(theta/2), cos(phi/2).
void FixedLightingConstants(const std::map<uint32_t, std::array<float, 16>>& transforms, const uint32_t* rs,
                            const guest::D3DMATERIAL9& material, const std::vector<const guest::D3DLIGHT9*>& lights,
                            float (*c)[4]);

// HLSL (vs_3_0, entry "main") writing exactly the pixel shader's inputs;
// its only constant is `float4x4 wvp` (c0-c3, row-major), or for POSITIONT
// `float4 vp` (c0: viewport x, y, width, height). Fatal for what
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
