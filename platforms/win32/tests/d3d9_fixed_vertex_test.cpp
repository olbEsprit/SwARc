// A pixel shader without a vertex shader (Direct3D 9 runs the fixed-
// function vertex stage in front of it): the generated vertex shader is
// drawn on the SDL_GPU device with a real pixel shader into the back
// buffer, and pixels are read back. Position is a FLOAT2 element, as
// vertex declarations may give it; the transform is WORLD * VIEW *
// PROJECTION; the pixel shader sees the declaration's texture coordinates.
// Exits 77 (ctest: skipped) where no GPU device can be created.

#include <SDL3/SDL.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "runtime/gfx/device.h"
#include "win32/d3d9_fixed_vertex.h"
#include "runtime/hlsl/hlsl.h"

using namespace recomp;

namespace {

constexpr int kSize = 64;
gfx::FfState g_state;
int failures = 0;

void Check(bool ok, const char* what, int x, int y, uint32_t got) {
  if (ok) return;
  std::fprintf(stderr, "FAIL %s at (%d,%d): %08X\n", what, x, y, got);
  failures++;
}

struct Vertex {
  float x, y;
  uint32_t color;
  float u, v;
};

uint32_t Pixel(gfx::Device& dev, int x, int y) {
  uint32_t px = 0;
  dev.ReadBack({x, y, x + 1, y + 1}, &px, 4, gfx::Format::A8R8G8B8);
  return px;
}

// Draws a quad covering clip space (scaled by `scale` through WORLD) and
// returns the generated vertex shader's source and the compile result.
void Draw(gfx::Device& dev, gfx::ShaderId ps_id, const hlsl::Program& ps, float scale) {
  std::vector<guest::D3DVERTEXELEMENT9> decl = {
      {0, 0, 1, 0, 0, 0},    // FLOAT2 POSITION
      {0, 8, 4, 0, 10, 0},   // D3DCOLOR COLOR0
      {0, 12, 1, 0, 5, 0},   // FLOAT2 TEXCOORD0
      {0xFF, 0, 17, 0, 0, 0},
  };
  uint32_t tss[8][33] = {};
  for (int s = 0; s < 8; s++) tss[s][11] = s;
  d3d9::FixedVertexState state{&decl, &ps.varyings, tss, false};
  std::string src = d3d9::FixedVertexSource(state);
  hlsl::Program vs;
  std::string errors;
  if (!hlsl::Compile(src, {}, "main", "vs_3_0", 0, &vs, &errors)) {
    std::fprintf(stderr, "generated vertex shader does not compile:\n%s\n%s", errors.c_str(), src.c_str());
    std::exit(1);
  }
  gfx::ShaderId vs_id = dev.CreateShader(vs);

  std::map<uint32_t, std::array<float, 16>> transforms;
  transforms[256] = {scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  float wvp[16];
  d3d9::WorldViewProjection(transforms, wvp);
  float constants[4][4];
  std::memcpy(constants, wvp, sizeof wvp);

  const Vertex quad[4] = {{-1, 1, 0xFFFFFFFF, 0, 0}, {1, 1, 0xFFFFFFFF, 1, 0},
                          {-1, -1, 0xFFFFFFFF, 0, 1}, {1, -1, 0xFFFFFFFF, 1, 1}};
  std::vector<gfx::VertexInput> inputs = d3d9::VertexInputs(vs.inputs, decl);
  gfx::ShaderDraw sd;
  sd.vs = vs_id;
  sd.ps = ps_id;
  sd.prim = 5;
  sd.primitives = 2;
  sd.vertex_count = 4;
  sd.inputs = inputs.data();
  sd.input_count = uint32_t(inputs.size());
  sd.streams[0] = {reinterpret_cast<const uint8_t*>(quad), sizeof quad, sizeof(Vertex)};
  sd.vs_float = constants;
  static const float zero[224][4] = {};
  sd.ps_float = zero;
  sd.state = &g_state;
  dev.Clear(0, nullptr, 1, 0xFF000000, 1.0f, 0);
  dev.BeginScene();
  dev.DrawShader(sd);
  dev.EndScene();
  dev.DestroyShader(vs_id);
}

void DrawPretransformed(gfx::Device& dev, gfx::ShaderId ps_id, const hlsl::Program& ps) {
  struct V { float x, y, z, rhw; uint32_t c; float u, v; };
  std::vector<guest::D3DVERTEXELEMENT9> decl = {
      {0, 0, 3, 0, 9, 0},    // FLOAT4 POSITIONT
      {0, 16, 4, 0, 10, 0},  // D3DCOLOR COLOR0
      {0, 20, 1, 0, 5, 0},   // FLOAT2 TEXCOORD0
      {0xFF, 0, 17, 0, 0, 0},
  };
  uint32_t tss[8][33] = {};
  for (int s = 0; s < 8; s++) tss[s][11] = s;
  d3d9::FixedVertexState state{&decl, &ps.varyings, tss, false};
  std::string src = d3d9::FixedVertexSource(state);
  hlsl::Program vs;
  std::string errors;
  if (!hlsl::Compile(src, {}, "main", "vs_3_0", 0, &vs, &errors)) {
    std::fprintf(stderr, "generated vertex shader does not compile:\n%s\n%s", errors.c_str(), src.c_str());
    std::exit(1);
  }
  gfx::ShaderId vs_id = dev.CreateShader(vs);
  const float s = float(kSize);
  float constants[4][4] = {{s / 2, s / 2, s / 2, s / 2}};
  const V quad[4] = {{s / 2, s / 2, 0, 1, 0xFFFFFFFF, 0.5f, 0.5f}, {s, s / 2, 0, 1, 0xFFFFFFFF, 0.5f, 0.5f},
                     {s / 2, s, 0, 1, 0xFFFFFFFF, 0.5f, 0.5f}, {s, s, 0, 1, 0xFFFFFFFF, 0.5f, 0.5f}};
  std::vector<gfx::VertexInput> inputs = d3d9::VertexInputs(vs.inputs, decl);
  gfx::ShaderDraw sd;
  sd.vs = vs_id;
  sd.ps = ps_id;
  sd.prim = 5;
  sd.primitives = 2;
  sd.vertex_count = 4;
  sd.inputs = inputs.data();
  sd.input_count = uint32_t(inputs.size());
  sd.streams[0] = {reinterpret_cast<const uint8_t*>(quad), sizeof quad, sizeof(V)};
  sd.vs_float = constants;
  static const float zero[224][4] = {};
  sd.ps_float = zero;
  gfx::FfState quadrant = g_state;
  quadrant.viewport = {kSize / 2, kSize / 2, kSize / 2, kSize / 2, 0.0f, 1.0f};
  sd.state = &quadrant;
  dev.Clear(0, nullptr, 1, 0xFF000000, 1.0f, 0);
  dev.BeginScene();
  dev.DrawShader(sd);
  dev.EndScene();
  dev.DestroyShader(vs_id);
}

// A lit quad covering clip space: normals toward the viewer (-z), one directional light shining along +z, so N.L
// is 1; COLOR0 read by `ps_lit`. With `vertex_diffuse` COLORVERTEX takes the diffuse from the vertex's colour.
void DrawLit(gfx::Device& dev, gfx::ShaderId ps_id, const hlsl::Program& ps, bool vertex_diffuse) {
  struct V { float x, y, z, nx, ny, nz; uint32_t c; };
  std::vector<guest::D3DVERTEXELEMENT9> decl = {
      {0, 0, 2, 0, 0, 0},    // FLOAT3 POSITION
      {0, 12, 2, 0, 3, 0},   // FLOAT3 NORMAL
      {0, 24, 4, 0, 10, 0},  // D3DCOLOR COLOR0
      {0xFF, 0, 17, 0, 0, 0},
  };
  uint32_t tss[8][33] = {};
  uint32_t rs[256] = {};
  rs[137] = 1;            // LIGHTING
  rs[139] = 0x00202020;   // AMBIENT
  rs[141] = vertex_diffuse ? 1 : 0;  // COLORVERTEX
  rs[145] = 1;            // DIFFUSEMATERIALSOURCE: COLOR1
  d3d9::FixedVertexState state{&decl, &ps.varyings, tss, true, rs, {3}};
  std::string src = d3d9::FixedVertexSource(state);
  hlsl::Program vs;
  std::string errors;
  if (!hlsl::Compile(src, {}, "main", "vs_3_0", 0, &vs, &errors)) {
    std::fprintf(stderr, "generated lit vertex shader does not compile:\n%s\n%s", errors.c_str(), src.c_str());
    std::exit(1);
  }
  gfx::ShaderId vs_id = dev.CreateShader(vs);
  std::map<uint32_t, std::array<float, 16>> transforms;
  float constants[256][4] = {};
  d3d9::WorldViewProjection(transforms, &constants[0][0]);
  guest::D3DMATERIAL9 mat{};
  mat.Diffuse = {1.0f, 0.5f, 0.0f, 1.0f};
  mat.Ambient = {1.0f, 1.0f, 1.0f, 1.0f};
  guest::D3DLIGHT9 light{};
  light.Type = 3;
  light.Diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
  light.Direction = {0.0f, 0.0f, 1.0f};
  d3d9::FixedLightingConstants(transforms, rs, mat, {&light}, constants);
  const V quad[4] = {{-1, 1, 0.5f, 0, 0, -1, 0xFF0000FF}, {1, 1, 0.5f, 0, 0, -1, 0xFF0000FF},
                     {-1, -1, 0.5f, 0, 0, -1, 0xFF0000FF}, {1, -1, 0.5f, 0, 0, -1, 0xFF0000FF}};
  std::vector<gfx::VertexInput> inputs = d3d9::VertexInputs(vs.inputs, decl);
  gfx::ShaderDraw sd;
  sd.vs = vs_id;
  sd.ps = ps_id;
  sd.prim = 5;
  sd.primitives = 2;
  sd.vertex_count = 4;
  sd.inputs = inputs.data();
  sd.input_count = uint32_t(inputs.size());
  sd.streams[0] = {reinterpret_cast<const uint8_t*>(quad), sizeof quad, sizeof(V)};
  sd.vs_float = constants;
  static const float zero[224][4] = {};
  sd.ps_float = zero;
  sd.state = &g_state;
  dev.Clear(0, nullptr, 1, 0xFF000000, 1.0f, 0);
  dev.BeginScene();
  dev.DrawShader(sd);
  dev.EndScene();
  dev.DestroyShader(vs_id);
}

}  // namespace

int main() {
  if (!SDL_Init(SDL_INIT_VIDEO)) return 77;
#if defined(__APPLE__)
  SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_METAL;
#else
  SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN;
#endif
  SDL_Window* win = SDL_CreateWindow("d3d9_fixed_vertex_test", kSize, kSize, flags);
  if (!win) return 77;
  std::unique_ptr<gfx::Device> dev = gfx::CreateGpuDevice(win);
  if (!dev) return 77;
  dev->ResetSwapChain(kSize, kSize, false);
  g_state = gfx::DefaultFfState();
  g_state.viewport = {0, 0, kSize, kSize, 0.0f, 1.0f};
  g_state.scissor = {0, 0, kSize, kSize};
  g_state.rs[7] = 0;   // ZENABLE
  g_state.rs[27] = 0;  // ALPHABLENDENABLE
  g_state.rs[22] = 1;  // CULLMODE none

  // Reads only TEXCOORD0, as a ps_1_1 shader doing `tex t0` does.
  hlsl::Program ps;
  std::string errors;
  const char* ps_src = "float4 main(float4 t0 : TEXCOORD0) : COLOR0 { return float4(t0.x, t0.y, 0.5, 1.0); }\n";
  if (!hlsl::Compile(ps_src, {}, "main", "ps_3_0", 0, &ps, &errors)) {
    std::fprintf(stderr, "%s\n", errors.c_str());
    return 1;
  }
  gfx::ShaderId ps_id = dev->CreateShader(ps);

  // Identity: the quad fills the target and the texture coordinates ramp.
  Draw(*dev, ps_id, ps, 1.0f);
  uint32_t c = Pixel(*dev, 16, 16);  // u = v = 0.25
  Check(std::abs(int(c >> 16 & 0xFF) - 64) <= 3 && std::abs(int(c >> 8 & 0xFF) - 64) <= 3 &&
            std::abs(int(c & 0xFF) - 128) <= 2,
        "texture coordinates through the generated vertex stage", 16, 16, c);
  c = Pixel(*dev, 48, 16);  // u = 0.75, v = 0.25
  Check(std::abs(int(c >> 16 & 0xFF) - 191) <= 3 && std::abs(int(c >> 8 & 0xFF) - 64) <= 3,
        "texture coordinates through the generated vertex stage", 48, 16, c);

  // WORLD scales to half: corners stay clear, the centre is drawn.
  Draw(*dev, ps_id, ps, 0.5f);
  c = Pixel(*dev, 4, 4);
  Check((c & 0xFFFFFF) == 0, "the world transform applies (outside)", 4, 4, c);
  c = Pixel(*dev, 32, 32);
  Check(std::abs(int(c & 0xFF) - 128) <= 2, "the world transform applies (inside)", 32, 32, c);

  // POSITIONT pixels of a quarter of the target, through a viewport offset by its quadrant.
  DrawPretransformed(*dev, ps_id, ps);
  c = Pixel(*dev, 8, 8);
  Check((c & 0xFFFFFF) == 0, "pretransformed: outside the quad", 8, 8, c);
  c = Pixel(*dev, 48, 48);
  Check(std::abs(int(c >> 16 & 0xFF) - 128) <= 8 && std::abs(int(c & 0xFF) - 128) <= 2,
        "pretransformed: inside the quad", 48, 48, c);

  // Lighting: the material's diffuse by the light plus its ambient by AMBIENT (0x20 = 32).
  hlsl::Program ps_lit;
  if (!hlsl::Compile("float4 main(float4 c : COLOR0) : COLOR0 { return c; }\n", {}, "main", "ps_3_0", 0, &ps_lit,
                     &errors)) {
    std::fprintf(stderr, "%s\n", errors.c_str());
    return 1;
  }
  gfx::ShaderId ps_lit_id = dev->CreateShader(ps_lit);
  DrawLit(*dev, ps_lit_id, ps_lit, false);
  c = Pixel(*dev, 32, 32);
  Check(int(c >> 16 & 0xFF) >= 253 && std::abs(int(c >> 8 & 0xFF) - 160) <= 3 && std::abs(int(c & 0xFF) - 32) <= 3,
        "lit: material diffuse x light + ambient", 32, 32, c);
  DrawLit(*dev, ps_lit_id, ps_lit, true);
  c = Pixel(*dev, 32, 32);
  Check(std::abs(int(c >> 16 & 0xFF) - 32) <= 3 && std::abs(int(c >> 8 & 0xFF) - 32) <= 3 && int(c & 0xFF) >= 253,
        "lit: COLORVERTEX takes the vertex's diffuse", 32, 32, c);
  dev->DestroyShader(ps_lit_id);

  dev.reset();
  SDL_DestroyWindow(win);
  SDL_Quit();
  if (failures) return 1;
  std::puts("ok");
  return 0;
}
