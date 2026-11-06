// A ps_1_x `tex t0` samples the texture bound at the stage, a cube here: the
// shader translated with the stage in `cube_stages` returns the cube face
// along t0's xyz, without it the 2D sampler finds a cube and reads black.
// Exits 77 (ctest: skipped) where no GPU device can be created.

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "runtime/gfx/device.h"
#include "runtime/hle/d3d9_fixed_vertex.h"
#include "runtime/hlsl/hlsl.h"

using namespace recomp;

namespace {

constexpr int kSize = 32;

struct Vertex {
  float x, y, tx, ty, tz;
};

uint32_t Draw(gfx::Device& dev, gfx::ShaderId vs_id, const hlsl::Program& vs, gfx::ShaderId ps_id,
              gfx::TextureId cube, float dx, float dy, float dz) {
  std::vector<guest::D3DVERTEXELEMENT9> decl = {
      {0, 0, 1, 0, 0, 0},  // FLOAT2 POSITION
      {0, 8, 2, 0, 5, 0},  // FLOAT3 TEXCOORD0
      {0xFF, 0, 17, 0, 0, 0},
  };
  const Vertex quad[4] = {{-1, 1, dx, dy, dz}, {1, 1, dx, dy, dz}, {-1, -1, dx, dy, dz},
                          {1, -1, dx, dy, dz}};
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
  static const float zero[256][4] = {};
  sd.vs_float = zero;
  sd.ps_float = zero;
  sd.textures[0] = cube;
  dev.Clear(0, nullptr, 1, 0xFF000000, 1.0f, 0);
  dev.BeginScene();
  dev.DrawShader(sd);
  dev.EndScene();
  uint32_t px = 0;
  dev.ReadBack({16, 16, 17, 17}, &px, 4, gfx::Format::A8R8G8B8);
  return px & 0xFFFFFF;
}

}  // namespace

int main() {
  if (!SDL_Init(SDL_INIT_VIDEO)) return 77;
#if defined(__APPLE__)
  SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_METAL;
#else
  SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN;
#endif
  SDL_Window* win = SDL_CreateWindow("gpu_sm1_cube_test", kSize, kSize, flags);
  if (!win) return 77;
  std::unique_ptr<gfx::Device> dev = gfx::CreateGpuDevice(win);
  if (!dev) return 77;
  dev->ResetSwapChain(kSize, kSize, false);
  dev->SetViewport({0, 0, kSize, kSize, 0.0f, 1.0f});
  dev->SetScissor({0, 0, kSize, kSize});
  dev->SetRenderState(7, 0);   // ZENABLE
  dev->SetRenderState(27, 0);  // ALPHABLENDENABLE
  dev->SetRenderState(22, 1);  // CULLMODE none

  gfx::TextureId cube = dev->CreateCubeTexture(4, 1, gfx::Format::A1R5G5B5, false);
  uint16_t red[16], blue[16];  // A1R5G5B5
  for (int i = 0; i < 16; i++) red[i] = 0xFC00, blue[i] = 0x801F;
  dev->UpdateCubeTexture(cube, 0, 0, {0, 0, 4, 4}, red, 8);   // +X
  dev->UpdateCubeTexture(cube, 5, 0, {0, 0, 4, 4}, blue, 8);  // -Z

  hlsl::Program vs;
  std::string errors;
  const char* vs_src =
      "struct O { float4 p : POSITION; float3 t : TEXCOORD0; };\n"
      "O main(float2 p : POSITION, float3 t : TEXCOORD0) { O o; o.p = float4(p, 0, 1); o.t = t; return o; }\n";
  if (!hlsl::Compile(vs_src, {}, "main", "vs_3_0", 0, &vs, &errors)) {
    std::fprintf(stderr, "%s\n", errors.c_str());
    return 1;
  }
  gfx::ShaderId vs_id = dev->CreateShader(vs);

  // ps_1_1: tex t0; mov r0, t0
  const uint32_t ps_tokens[] = {0xFFFF0101, 0x42, 0xB00F0000, 0x1, 0x800F0000, 0xB0E40000, 0xFFFF};
  int failures = 0;
  for (uint32_t cube_stages : {1u, 0u}) {
    hlsl::Program ps;
    if (!hlsl::TranslateBytecode(ps_tokens, std::size(ps_tokens), &ps, &errors, cube_stages)) {
      std::fprintf(stderr, "%s\n", errors.c_str());
      return 1;
    }
    gfx::ShaderId ps_id = dev->CreateShader(ps);
    uint32_t px = Draw(*dev, vs_id, vs, ps_id, cube, 1, 0, 0);
    uint32_t px2 = Draw(*dev, vs_id, vs, ps_id, cube, 0, 0, -1);
    uint32_t want = cube_stages ? 0xFF0000 : 0, want2 = cube_stages ? 0x0000FF : 0;
    if (px != want || px2 != want2) {
      std::fprintf(stderr, "FAIL cube_stages %u: +X %06X (wanted %06X), -Z %06X (wanted %06X)\n",
                   cube_stages, px, want, px2, want2);
      failures++;
    }
    dev->DestroyShader(ps_id);
  }
  dev->DestroyShader(vs_id);
  dev.reset();
  SDL_DestroyWindow(win);
  SDL_Quit();
  if (failures) return 1;
  std::puts("ok");
  return 0;
}
