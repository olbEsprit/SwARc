// Cube textures in the fixed-function pipeline on the SDL_GPU device: a
// texture stage with a cube map samples it with the stage's 3-component
// texture coordinate as a direction, and a cube face can be a render target
// (cube maps rendered at run time). Faces get different colours (uploaded,
// or cleared while bound as the target) and are read back through draws
// into the back buffer. Exits 77 (ctest: skipped) where no GPU device can
// be created.

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <memory>

#include "runtime/gfx/device.h"

using namespace recomp;

namespace {

constexpr int kSize = 32;
int failures = 0;

struct Vertex {
  float x, y, z;
  float tx, ty, tz;
};

// D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0)
constexpr uint32_t kFvf = 0x002 | 0x100 | (1u << 16);

// Fills the back buffer with the cube map seen along `dir`, reads (16, 16).
uint32_t Sample(gfx::Device& dev, gfx::TextureId cube, float dx, float dy, float dz) {
  const Vertex quad[4] = {{-1, 1, 0, dx, dy, dz}, {1, 1, 0, dx, dy, dz},
                          {-1, -1, 0, dx, dy, dz}, {1, -1, 0, dx, dy, dz}};
  gfx::ColorTarget back[4] = {};
  dev.SetRenderTargets(back, 0);
  dev.SetTexture(0, cube);
  dev.Clear(0, nullptr, 1, 0xFF000000, 1.0f, 0);
  dev.BeginScene();
  dev.Draw(5, kFvf, quad, 4, sizeof(Vertex), nullptr, 0);
  dev.EndScene();
  uint32_t px = 0;
  dev.ReadBack({16, 16, 17, 17}, &px, 4, gfx::Format::A8R8G8B8);
  return px & 0xFFFFFF;
}

void Expect(uint32_t got, uint32_t want, const char* what) {
  bool ok = true;
  for (int shift : {0, 8, 16})
    ok = ok && std::abs(int(got >> shift & 0xFF) - int(want >> shift & 0xFF)) <= 4;
  if (ok) return;
  std::fprintf(stderr, "FAIL %s: %06X, wanted %06X\n", what, got, want);
  failures++;
}

}  // namespace

int main() {
  if (!SDL_Init(SDL_INIT_VIDEO)) return 77;
#if defined(__APPLE__)
  SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_METAL;
#else
  SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN;
#endif
  SDL_Window* win = SDL_CreateWindow("gpu_cube_ff_test", kSize, kSize, flags);
  if (!win) return 77;
  std::unique_ptr<gfx::Device> dev = gfx::CreateGpuDevice(win);
  if (!dev) return 77;
  dev->ResetSwapChain(kSize, kSize, false);
  dev->SetViewport({0, 0, kSize, kSize, 0.0f, 1.0f});
  dev->SetScissor({0, 0, kSize, kSize});
  dev->SetRenderState(7, 0);    // ZENABLE
  dev->SetRenderState(22, 1);   // CULLMODE none
  dev->SetRenderState(27, 0);   // ALPHABLENDENABLE
  dev->SetRenderState(137, 0);  // LIGHTING
  dev->SetTextureStageState(0, 1, 2);  // COLOROP SELECTARG1
  dev->SetTextureStageState(0, 2, 2);  // COLORARG1 TEXTURE
  dev->SetTextureStageState(0, 4, 2);  // ALPHAOP SELECTARG1
  dev->SetTextureStageState(0, 5, 2);  // ALPHAARG1 TEXTURE
  dev->SetTextureStageState(1, 1, 1);  // COLOROP DISABLE
  dev->SetTextureStageState(1, 4, 1);

  // Faces +X (0) and -Z (5) uploaded, +Y (2) and -Y (3) rendered into.
  gfx::TextureId cube = dev->CreateCubeTexture(4, 1, gfx::Format::A1R5G5B5, true);
  if (!cube) {
    std::fprintf(stderr, "FAIL CreateCubeTexture\n");
    return 1;
  }
  uint16_t red[16], blue[16];  // A1R5G5B5
  for (int i = 0; i < 16; i++) red[i] = 0xFC00, blue[i] = 0x801F;
  dev->UpdateCubeTexture(cube, 0, 0, {0, 0, 4, 4}, red, 8);
  dev->UpdateCubeTexture(cube, 5, 0, {0, 0, 4, 4}, blue, 8);
  for (auto [face, argb] : {std::pair{2, 0xFF00FF00u}, std::pair{3, 0xFFFFFF00u}}) {
    gfx::ColorTarget target[4] = {};
    target[0] = {cube, 0, face};
    dev->SetRenderTargets(target, 0);
    dev->SetViewport({0, 0, 4, 4, 0.0f, 1.0f});
    dev->SetScissor({0, 0, 4, 4});
    dev->BeginScene();
    dev->Clear(0, nullptr, 1, argb, 1.0f, 0);
    dev->EndScene();
  }
  dev->SetViewport({0, 0, kSize, kSize, 0.0f, 1.0f});
  dev->SetScissor({0, 0, kSize, kSize});

  Expect(Sample(*dev, cube, 1, 0, 0), 0xFF0000, "+X face, uploaded");
  Expect(Sample(*dev, cube, 0, 0, -1), 0x0000FF, "-Z face, uploaded");
  Expect(Sample(*dev, cube, 0, 1, 0), 0x00FF00, "+Y face, rendered into");
  Expect(Sample(*dev, cube, 0, -1, 0), 0xFFFF00, "-Y face, rendered into");

  dev.reset();
  SDL_DestroyWindow(win);
  SDL_Quit();
  if (failures) return 1;
  std::puts("ok");
  return 0;
}
