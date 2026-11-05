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
#include "runtime/hle/d3d9_fixed_vertex.h"
#include "runtime/hlsl/hlsl.h"

using namespace recomp;

namespace {

constexpr int kSize = 64;
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
  dev->SetViewport({0, 0, kSize, kSize, 0.0f, 1.0f});
  dev->SetScissor({0, 0, kSize, kSize});
  dev->SetRenderState(7, 0);   // ZENABLE
  dev->SetRenderState(27, 0);  // ALPHABLENDENABLE
  dev->SetRenderState(22, 1);  // CULLMODE none

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

  dev.reset();
  SDL_DestroyWindow(win);
  SDL_Quit();
  if (failures) return 1;
  std::puts("ok");
  return 0;
}
