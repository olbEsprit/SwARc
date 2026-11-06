#pragma once
// MMX integer instructions for lifted code. mm0-mm7 are the low 64 bits of
// the x87 registers R0-R7 (not ST(i)): every MMX instruction but EMMS sets
// TOP to 0 and marks the registers valid, and writing an mm register sets
// the sign and exponent of its R to all ones. Tags are derived from the
// contents (x87.h), so while MMX is live they read as "special", not
// "valid"; only code that does FNSAVE/FNSTENV before EMMS could tell.

#include <cstdint>
#include <type_traits>

#include "runtime/ctx.h"

namespace recomp::mmx {

inline void Enter(Ctx& c) {
  c.fpu.top = 0;
  c.fpu.empty = 0;
}
inline void Emms(Ctx& c) { c.fpu.empty = 0xFF; }
inline void Set(Ctx& c, int i, uint64_t v) {
  c.fpu.r[i].sig = v;
  c.fpu.r[i].se = 0xFFFF;
}

namespace detail {
template <typename T, typename F>
inline uint64_t Lanes(uint64_t a, uint64_t b, F f) {
  constexpr int kBits = 8 * sizeof(T);
  uint64_t r = 0;
  for (int i = 0; i < 64; i += kBits)
    r |= uint64_t(std::make_unsigned_t<T>(f(static_cast<T>(a >> i), static_cast<T>(b >> i)))) << i;
  return r;
}
inline int16_t SatS16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : int16_t(v); }
inline uint8_t SatU8(int16_t v) { return v < 0 ? 0 : v > 255 ? 255 : uint8_t(v); }
template <typename T>
inline uint64_t Unpack(uint64_t a, uint64_t b, int shift) {
  constexpr int kBits = 8 * sizeof(T);
  uint64_t r = 0;
  for (int i = 0; i < 32 / kBits; i++) {
    r |= ((a >> (shift + i * kBits)) & ((uint64_t(1) << kBits) - 1)) << (2 * i * kBits);
    r |= ((b >> (shift + i * kBits)) & ((uint64_t(1) << kBits) - 1)) << ((2 * i + 1) * kBits);
  }
  return r;
}
}  // namespace detail

inline uint64_t Paddb(uint64_t a, uint64_t b) {
  return detail::Lanes<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return uint8_t(x + y); });
}
inline uint64_t Paddw(uint64_t a, uint64_t b) {
  return detail::Lanes<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return uint16_t(x + y); });
}
inline uint64_t Paddd(uint64_t a, uint64_t b) {
  return detail::Lanes<uint32_t>(a, b, [](uint32_t x, uint32_t y) { return x + y; });
}
inline uint64_t Psubw(uint64_t a, uint64_t b) {
  return detail::Lanes<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return uint16_t(x - y); });
}
inline uint64_t Psubd(uint64_t a, uint64_t b) {
  return detail::Lanes<uint32_t>(a, b, [](uint32_t x, uint32_t y) { return x - y; });
}
inline uint64_t Paddsw(uint64_t a, uint64_t b) {
  return detail::Lanes<int16_t>(a, b, [](int16_t x, int16_t y) { return detail::SatS16(x + y); });
}
inline uint64_t Psubsw(uint64_t a, uint64_t b) {
  return detail::Lanes<int16_t>(a, b, [](int16_t x, int16_t y) { return detail::SatS16(x - y); });
}
inline uint64_t Pmaxsw(uint64_t a, uint64_t b) {
  return detail::Lanes<int16_t>(a, b, [](int16_t x, int16_t y) { return x > y ? x : y; });
}
inline uint64_t Pcmpeqw(uint64_t a, uint64_t b) {
  return detail::Lanes<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return uint16_t(x == y ? 0xFFFF : 0); });
}
inline uint64_t Pcmpeqd(uint64_t a, uint64_t b) {
  return detail::Lanes<uint32_t>(a, b, [](uint32_t x, uint32_t y) { return x == y ? 0xFFFFFFFFu : 0u; });
}
inline uint64_t Pcmpgtd(uint64_t a, uint64_t b) {
  return detail::Lanes<int32_t>(a, b, [](int32_t x, int32_t y) { return x > y ? -1 : 0; });
}
inline uint64_t Pmullw(uint64_t a, uint64_t b) {
  return detail::Lanes<int16_t>(a, b, [](int16_t x, int16_t y) { return int16_t(x * y); });
}
inline uint64_t Pmulhw(uint64_t a, uint64_t b) {
  return detail::Lanes<int16_t>(a, b, [](int16_t x, int16_t y) { return int16_t((x * y) >> 16); });
}
// Each pair of word products summed into a dword (wraps for 0x8000 * 0x8000 twice).
inline uint64_t Pmaddwd(uint64_t a, uint64_t b) {
  uint64_t r = 0;
  for (int i = 0; i < 64; i += 32) {
    const int32_t lo = int32_t(int16_t(a >> i)) * int16_t(b >> i);
    const int32_t hi = int32_t(int16_t(a >> (i + 16))) * int16_t(b >> (i + 16));
    r |= uint64_t(uint32_t(lo) + uint32_t(hi)) << i;
  }
  return r;
}
inline uint64_t Packssdw(uint64_t a, uint64_t b) {
  uint64_t r = 0;
  for (int i = 0; i < 2; i++) {
    r |= uint64_t(uint16_t(detail::SatS16(int32_t(a >> (32 * i))))) << (16 * i);
    r |= uint64_t(uint16_t(detail::SatS16(int32_t(b >> (32 * i))))) << (16 * (i + 2));
  }
  return r;
}
inline uint64_t Packsswb(uint64_t a, uint64_t b) {
  auto sat = [](int16_t v) { return uint8_t(v < -128 ? -128 : v > 127 ? 127 : v); };
  uint64_t r = 0;
  for (int i = 0; i < 4; i++) {
    r |= uint64_t(sat(int16_t(a >> (16 * i)))) << (8 * i);
    r |= uint64_t(sat(int16_t(b >> (16 * i)))) << (8 * (i + 4));
  }
  return r;
}
inline uint64_t Packuswb(uint64_t a, uint64_t b) {
  uint64_t r = 0;
  for (int i = 0; i < 4; i++) {
    r |= uint64_t(detail::SatU8(int16_t(a >> (16 * i)))) << (8 * i);
    r |= uint64_t(detail::SatU8(int16_t(b >> (16 * i)))) << (8 * (i + 4));
  }
  return r;
}
inline uint64_t Punpcklbw(uint64_t a, uint64_t b) { return detail::Unpack<uint8_t>(a, b, 0); }
inline uint64_t Punpcklwd(uint64_t a, uint64_t b) { return detail::Unpack<uint16_t>(a, b, 0); }
inline uint64_t Punpckldq(uint64_t a, uint64_t b) { return detail::Unpack<uint32_t>(a, b, 0); }
inline uint64_t Punpckhbw(uint64_t a, uint64_t b) { return detail::Unpack<uint8_t>(a, b, 32); }
inline uint64_t Punpckhwd(uint64_t a, uint64_t b) { return detail::Unpack<uint16_t>(a, b, 32); }
inline uint64_t Punpckhdq(uint64_t a, uint64_t b) { return detail::Unpack<uint32_t>(a, b, 32); }
inline uint64_t Pandn(uint64_t a, uint64_t b) { return ~a & b; }
inline uint64_t Pand(uint64_t a, uint64_t b) { return a & b; }
inline uint64_t Por(uint64_t a, uint64_t b) { return a | b; }
inline uint64_t Pxor(uint64_t a, uint64_t b) { return a ^ b; }

// Shifts by an immediate count; a count past the lane width gives zero
// (sign fill for arithmetic shifts).
inline uint64_t Psllw(uint64_t a, unsigned n) {
  return n > 15 ? 0 : detail::Lanes<uint16_t>(a, 0, [n](uint16_t x, uint16_t) { return uint16_t(x << n); });
}
inline uint64_t Pslld(uint64_t a, unsigned n) {
  return n > 31 ? 0 : detail::Lanes<uint32_t>(a, 0, [n](uint32_t x, uint32_t) { return x << n; });
}
inline uint64_t Psrld(uint64_t a, unsigned n) {
  return n > 31 ? 0 : detail::Lanes<uint32_t>(a, 0, [n](uint32_t x, uint32_t) { return x >> n; });
}
inline uint64_t Psllq(uint64_t a, unsigned n) { return n > 63 ? 0 : a << n; }
inline uint64_t Psrlw(uint64_t a, unsigned n) {
  return n > 15 ? 0 : detail::Lanes<uint16_t>(a, 0, [n](uint16_t x, uint16_t) { return uint16_t(x >> n); });
}
inline uint64_t Psrlq(uint64_t a, unsigned n) { return n > 63 ? 0 : a >> n; }
inline uint64_t Psraw(uint64_t a, unsigned n) {
  return detail::Lanes<int16_t>(a, 0, [n](int16_t x, int16_t) { return int16_t(x >> (n > 15 ? 15 : n)); });
}
inline uint64_t Psrad(uint64_t a, unsigned n) {
  return detail::Lanes<int32_t>(a, 0, [n](int32_t x, int32_t) { return int32_t(x >> (n > 31 ? 31 : n)); });
}

}  // namespace recomp::mmx
