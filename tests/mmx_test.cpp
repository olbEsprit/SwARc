// runtime/mmx.h on known results (saturation, wrap-around, lane order,
// shift counts past the lane width) and its x87 register aliasing. The
// bit-for-bit oracle against the CPU is lift_mmx_test.

#include <cstdint>
#include <cstdio>

#include "runtime/mmx.h"

namespace {

int g_fail = 0;

void Check(const char* what, uint64_t got, uint64_t want) {
  if (got == want) return;
  g_fail++;
  std::printf("FAIL %s: got %016llX want %016llX\n", what, (unsigned long long)got,
              (unsigned long long)want);
}

}  // namespace

int main() {
  using namespace recomp::mmx;

  Check("paddsw", Paddsw(0x7FFF8000'0001FFFFull, 0x0001FFFF'00010001ull), 0x7FFF8000'00020000ull);
  Check("psubsw", Psubsw(0x80007FFF'00000005ull, 0x0001FFFF'00000008ull), 0x80007FFF'0000FFFDull);
  Check("paddb wraps", Paddb(0xFF01'7F80'0000'FFFFull, 0x0101'0101'0101'0101ull), 0x0002'8081'0101'0000ull);
  Check("paddd wraps", Paddd(0xFFFFFFFF'7FFFFFFFull, 0x00000001'00000001ull), 0x00000000'80000000ull);
  Check("psubw", Psubw(0x0000'0001'8000'0005ull, 0x0001'0002'0001'0008ull), 0xFFFF'FFFF'7FFF'FFFDull);
  Check("pmaddwd wraps", Pmaddwd(0x8000800080008000ull, 0x8000800080008000ull), 0x80000000'80000000ull);
  Check("pmaddwd", Pmaddwd(0x0004'0003'0002'0001ull, 0x0008'0007'0006'0005ull), 0x00000035'00000011ull);
  Check("pmulhw", Pmulhw(0xFFFF8000'7FFF0002ull, 0xFFFF8000'7FFF4000ull), 0x00004000'3FFF0000ull);
  Check("pmullw", Pmullw(0xFFFF8000'7FFF0002ull, 0xFFFF8000'7FFF4000ull), 0x00010000'00018000ull);
  Check("packssdw", Packssdw(0x00012345'FFFF0000ull, 0x00007FFF'FFFF8000ull), 0x7FFF8000'7FFF8000ull);
  Check("packsswb", Packsswb(0x8000'0080'007F'FFFFull, 0x0004'0003'0002'0001ull), 0x04030201'807F7FFFull);
  Check("packuswb", Packuswb(0x8000'007F'0100'FFFFull, 0x0004'0003'0002'0001ull), 0x04030201'007FFF00ull);
  Check("punpcklbw", Punpcklbw(0x0807060504030201ull, 0x1817161514131211ull), 0x1404130312021101ull);
  Check("punpckhbw", Punpckhbw(0x0807060504030201ull, 0x1817161514131211ull), 0x1808170716061505ull);
  Check("punpcklwd", Punpcklwd(0x0004'0003'0002'0001ull, 0x0014'0013'0012'0011ull), 0x0012'0002'0011'0001ull);
  Check("punpckhwd", Punpckhwd(0x0004'0003'0002'0001ull, 0x0014'0013'0012'0011ull), 0x0014'0004'0013'0003ull);
  Check("punpckldq", Punpckldq(0x1111111122222222ull, 0x3333333344444444ull), 0x4444444422222222ull);
  Check("punpckhdq", Punpckhdq(0x1111111122222222ull, 0x3333333344444444ull), 0x3333333311111111ull);
  Check("psraw past width", Psraw(0x8000'7FFF'FFFF'0001ull, 20), 0xFFFF'0000'FFFF'0000ull);
  Check("psrad past width", Psrad(0x80000000'7FFFFFFFull, 31), 0xFFFFFFFF'00000000ull);
  Check("psllw", Psllw(0x0001000100010001ull, 15), 0x8000800080008000ull);
  Check("psllw past width", Psllw(0xFFFFFFFFFFFFFFFFull, 16), 0);
  Check("pslld", Pslld(0x00000001'80000001ull, 1), 0x00000002'00000002ull);
  Check("psllq past width", Psllq(1, 64), 0);
  Check("psrlw", Psrlw(0x8000'FFFF'0001'0002ull, 1), 0x4000'7FFF'0000'0001ull);
  Check("psrld", Psrld(0x80000000'FFFFFFFFull, 4), 0x08000000'0FFFFFFFull);
  Check("psrlq past width", Psrlq(~0ull, 64), 0);
  Check("pcmpgtd", Pcmpgtd(0x00000002'FFFFFFFFull, 0x00000001'00000000ull), 0xFFFFFFFF'00000000ull);
  Check("pcmpeqw", Pcmpeqw(0x0004'0003'0002'0001ull, 0x0004'0000'0002'FFFFull), 0xFFFF'0000'FFFF'0000ull);
  Check("pcmpeqd", Pcmpeqd(0x00000002'00000001ull, 0x00000002'00000000ull), 0xFFFFFFFF'00000000ull);
  Check("pmaxsw", Pmaxsw(0x8000'7FFF'FFFF'0001ull, 0x0001'8000'0000'FFFEull), 0x0001'7FFF'0000'0001ull);
  Check("pandn", Pandn(0xFF00FF00FF00FF00ull, 0x123456789ABCDEF0ull), 0x00340078'00BC00F0ull);

  recomp::Ctx c{};
  c.fpu.top = 5;
  c.fpu.empty = 0xFF;
  Enter(c);
  Set(c, 3, 0x1122334455667788ull);
  Check("enter top", c.fpu.top, 0);
  Check("enter tags", c.fpu.empty, 0);
  Check("set sig", c.fpu.r[3].sig, 0x1122334455667788ull);
  Check("set exponent", c.fpu.r[3].se, 0xFFFF);
  Emms(c);
  Check("emms tags", c.fpu.empty, 0xFF);
  Check("emms keeps data", c.fpu.r[3].sig, 0x1122334455667788ull);

  std::printf("mmx: %d failures\n", g_fail);
  return g_fail ? 1 : 0;
}
