// sse::RcpLane (RCPPS/RCPSS) is the correctly rounded 1/x (see sse.h).
//
// - Every significand at exponent 127 (1 <= x < 2), checked against the
//   two midpoints around the result with exact integer arithmetic.
// - The scaling identity rcp(x * 2^k) = rcp(x) * 2^-k over the whole range
//   where the result is normal.
// - The architectural error bound, |rel. error| <= 1.5 * 2^-12.
// - Special operands: ±0, denormals, tiny results, ±inf, NaNs.

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "runtime/sse.h"

namespace recomp {
void FatalF(const char* fmt, ...) {  // runtime/sse.cpp's #XM
  va_list ap;
  va_start(ap, fmt);
  std::vprintf(fmt, ap);
  va_end(ap);
  std::abort();
}
}  // namespace recomp

namespace {

using u128 = unsigned __int128;

int g_fail = 0;

void Fail(const char* what, uint32_t x, uint32_t got) {
  if (++g_fail <= 20) std::printf("FAIL %s: x=%08X got %08X\n", what, x, got);
}

float F(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// True if the positive normal float r is the float nearest to 1/x:
// lo * x < 1 < hi * x for the midpoints lo, hi to r's neighbours.
bool Nearest(uint32_t x, uint32_t r) {
  const u128 mx = (x & 0x7FFFFF) | 0x800000, mr = (r & 0x7FFFFF) | 0x800000;
  const int e = int(x >> 23) - 150 + int(r >> 23) - 150 - 2;  // units of 2^(er-2)
  const u128 lo = 4 * mr - (mr == 0x800000 ? 1 : 2), hi = 4 * mr + 2;
  const u128 one = u128(1) << -e;
  return lo * mx < one && hi * mx > one;
}

}  // namespace

int main() {
  using recomp::sse::RcpLane;

  uint64_t n = 0;
  double worst = 0;
  for (uint32_t m = 0; m < 0x800000; m++) {
    const uint32_t x = (127u << 23) | m, r = RcpLane(x);
    if (!Nearest(x, r)) Fail("nearest", x, r);
    if (Nearest(x, r - 1) || Nearest(x, r + 1)) Fail("check rejects neighbours", x, r);
    const double rel = std::fabs(double(F(r)) * double(F(x)) - 1.0);
    if (rel > worst) worst = rel;
    n++;
  }
  if (worst > 1.5 / 4096) Fail("error bound", 0, 0);

  for (uint32_t m = 0; m < 0x800000; m += 4093) {
    const uint32_t base = RcpLane((127u << 23) | m);
    for (int e = 1; e <= 252; e++) {
      const uint32_t x = (uint32_t(e) << 23) | m;
      if (x >= 0x7E7FE800u) continue;
      const uint32_t want = base - (uint32_t(e - 127) << 23);
      const uint32_t got = RcpLane(x);
      if (got != want) Fail("scaling", x, got);
    }
  }

  struct {
    uint32_t x, want;
  } special[] = {
      {0x00000000, 0x7F800000}, {0x80000000, 0xFF800000},  // ±0 -> ±inf
      {0x00000001, 0x7F800000}, {0x807FFFFF, 0xFF800000},  // denormals as ±0
      {0x3F800000, 0x3F800000}, {0x40000000, 0x3F000000},  // 1 -> 1, 2 -> 0.5
      {0xC0000000, 0xBF000000},                            // sign kept
      {0x7F800000, 0x00000000}, {0xFF800000, 0x80000000},  // ±inf -> ±0
      {0x7E7FE800, 0x00000000}, {0xFE7FE800, 0x80000000},  // tiny result: ±0
      {0x7F7FFFFF, 0x00000000},
      {0x7E7FE7FF, 0x00800C02},                            // just below: normal
      {0x00800000, 0x7E800000},                            // smallest normal -> 2^126
      {0x7FC00001, 0x7FC00001}, {0x7F800001, 0x7FC00001},  // QNaN, SNaN quieted
      {0xFF800001, 0xFFC00001},
  };
  for (const auto& s : special) {
    const uint32_t got = RcpLane(s.x);
    if (got != s.want) Fail("special", s.x, got);
  }

  std::printf("rcp: %llu significands, worst rel. error %.3g (bound %.3g), %d failures\n",
              (unsigned long long)n, worst, 1.5 / 4096, g_fail);
  return g_fail ? 1 : 0;
}
