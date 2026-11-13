// wsock32's ordinal imports through the thunk dispatch, on loopback: the
// byte-order, address and descriptor-set helpers, and a TCP connection
// (bind, listen, connect, select, accept, send, recv, getsockname,
// getsockopt, shutdown, closesocket). The guest process is a bare guest
// memory arena with the kernel32 state (SOCKET handles, the process heap).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "x86/ctx.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/process.h"

using namespace recomp;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", what);
    failures++;
  }
}

constexpr gaddr kStack = 0x00300000, kScratch = 0x00310000;
Ctx g_ctx;

// Calls wsock32.dll!#ord like a stdcall guest: args pushed right to left.
uint32_t Call(int ord, std::vector<uint32_t> args) {
  gaddr sp = kStack + 0x8000;
  for (size_t i = args.size(); i-- > 0;) {
    sp -= 4;
    g_proc->mem.write<uint32_t>(sp, args[i]);
  }
  sp -= 4;
  g_proc->mem.write<uint32_t>(sp, 0x1234);  // return address
  g_ctx.esp = uint32_t(sp);
  hle::CallThunk(g_ctx, hle::ImportThunk("wsock32.dll", "#" + std::to_string(ord)));
  return g_ctx.eax;
}

enum {
  ACCEPT = 1, BIND = 2, CLOSESOCKET = 3, CONNECT = 4, GETSOCKNAME = 6, GETSOCKOPT = 7,
  HTONL = 8, HTONS = 9, INET_ADDR = 11, INET_NTOA = 12, LISTEN = 13, NTOHL = 14, NTOHS = 15,
  RECV = 16, SELECT = 18, SEND = 19, SETSOCKOPT = 21, SHUTDOWN = 22, SOCKET = 23,
  WSAFDISSET = 151,
};

void Str(gaddr va, const char* s) { std::memcpy(GPtr(va), s, std::strlen(s) + 1); }

}  // namespace

int main() {
  static Process proc;
  g_proc = &proc;
  static Win32Process win32_proc;
  g_win32 = &win32_proc;
  if (!proc.mem.Reserve()) return 1;
  proc.vm.Init(&proc.mem, 0x10000, 0x70000000);
  proc.mem.Commit(kPebVa & ~0xFFFu, 0x2000, Prot::RW);
  proc.mem.Commit(kStack, 0x20000, Prot::RW);
  InitKernel32State();

  Check(Call(HTONS, {0x1234}) == 0x3412, "htons");
  Check(Call(NTOHS, {0x3412}) == 0x1234, "ntohs");
  Check(Call(HTONL, {0x11223344}) == 0x44332211, "htonl");
  Check(Call(NTOHL, {0x44332211}) == 0x11223344, "ntohl");

  Str(kScratch, "127.0.0.1");
  Check(Call(INET_ADDR, {uint32_t(kScratch)}) == 0x0100007F, "inet_addr");
  Str(kScratch, "not an address");
  Check(Call(INET_ADDR, {uint32_t(kScratch)}) == 0xFFFFFFFF, "inet_addr rejects text");
  gaddr text = Call(INET_NTOA, {0x0A02A8C0});
  Check(text && std::string(reinterpret_cast<char*>(GPtr(text))) == "192.168.2.10", "inet_ntoa");

  // A listening socket on loopback, port chosen by the host.
  uint32_t server = Call(SOCKET, {2, 1, 0});  // AF_INET, SOCK_STREAM
  Check(server != 0xFFFFFFFF, "socket");
  gaddr sa = kScratch + 0x100, len = kScratch + 0x140;
  std::memset(GPtr(sa), 0, 16);
  g_proc->mem.write<uint16_t>(sa, 2);
  g_proc->mem.write<uint32_t>(sa + 4, 0x0100007F);
  Check(Call(BIND, {server, uint32_t(sa), 16}) == 0, "bind");
  Check(Call(LISTEN, {server, 0x7FFFFFFF}) == 0, "listen");
  g_proc->mem.write<uint32_t>(len, 16);
  Check(Call(GETSOCKNAME, {server, uint32_t(sa), uint32_t(len)}) == 0, "getsockname");
  Check(g_proc->mem.read<uint32_t>(len) == 16 && g_proc->mem.read<uint16_t>(sa + 2) != 0,
        "getsockname gives the port");

  uint32_t client = Call(SOCKET, {2, 1, 0});
  Check(Call(CONNECT, {client, uint32_t(sa), 16}) == 0, "connect");

  // The listener is readable once the connection is pending.
  gaddr rset = kScratch + 0x200, tv = kScratch + 0x300;
  g_proc->mem.write<uint32_t>(rset, 1);
  g_proc->mem.write<uint32_t>(rset + 4, server);
  g_proc->mem.write<uint32_t>(tv, 2);
  g_proc->mem.write<uint32_t>(tv + 4, 0);
  Check(Call(SELECT, {0, uint32_t(rset), 0, 0, uint32_t(tv)}) == 1, "select: pending connection");
  Check(Call(WSAFDISSET, {server, uint32_t(rset)}) == 1, "__WSAFDIsSet: in the set");
  Check(Call(WSAFDISSET, {client, uint32_t(rset)}) == 0, "__WSAFDIsSet: not in the set");

  std::memset(GPtr(sa), 0, 16);
  g_proc->mem.write<uint32_t>(len, 16);
  uint32_t peer = Call(ACCEPT, {server, uint32_t(sa), uint32_t(len)});
  Check(peer != 0xFFFFFFFF && g_proc->mem.read<uint16_t>(sa) == 2 &&
            g_proc->mem.read<uint32_t>(sa + 4) == 0x0100007F,
        "accept");

  // Nothing to read yet: select times out with an empty set.
  g_proc->mem.write<uint32_t>(rset, 1);
  g_proc->mem.write<uint32_t>(rset + 4, peer);
  g_proc->mem.write<uint32_t>(tv, 0);
  g_proc->mem.write<uint32_t>(tv + 4, 10000);
  Check(Call(SELECT, {0, uint32_t(rset), 0, 0, uint32_t(tv)}) == 0, "select: timeout");
  Check(g_proc->mem.read<uint32_t>(rset) == 0, "select empties the set on timeout");

  gaddr buf = kScratch + 0x400;
  Str(buf, "hello");
  Check(Call(SEND, {client, uint32_t(buf), 5, 0}) == 5, "send");
  g_proc->mem.write<uint32_t>(rset, 1);
  g_proc->mem.write<uint32_t>(rset + 4, peer);
  g_proc->mem.write<uint32_t>(tv, 2);
  Check(Call(SELECT, {0, uint32_t(rset), 0, 0, uint32_t(tv)}) == 1, "select: data");
  std::memset(GPtr(buf), 0, 16);
  Check(Call(RECV, {peer, uint32_t(buf), 16, 0}) == 5 &&
            std::string(reinterpret_cast<char*>(GPtr(buf))) == "hello",
        "recv");

  // Options: sizes round trip through the host (which may round them),
  // SO_ERROR reads 0.
  g_proc->mem.write<uint32_t>(buf, 65536);
  Check(Call(SETSOCKOPT, {client, 0xFFFF, 0x1002, uint32_t(buf), 4}) == 0, "setsockopt SO_RCVBUF");
  g_proc->mem.write<uint32_t>(len, 4);
  Check(Call(GETSOCKOPT, {client, 0xFFFF, 0x1002, uint32_t(buf + 8), uint32_t(len)}) == 0 &&
            g_proc->mem.read<uint32_t>(buf + 8) >= 65536,
        "getsockopt SO_RCVBUF");
  g_proc->mem.write<uint32_t>(len, 4);
  Check(Call(GETSOCKOPT, {client, 0xFFFF, 0x1007, uint32_t(buf + 8), uint32_t(len)}) == 0 &&
            g_proc->mem.read<uint32_t>(buf + 8) == 0,
        "getsockopt SO_ERROR");

  // Shutting down the sender's side ends the stream: recv returns 0.
  Check(Call(SHUTDOWN, {client, 1}) == 0, "shutdown");
  Check(Call(RECV, {peer, uint32_t(buf), 16, 0}) == 0, "recv after shutdown");
  Check(Call(CLOSESOCKET, {client}) == 0 && Call(CLOSESOCKET, {peer}) == 0 &&
            Call(CLOSESOCKET, {server}) == 0,
        "closesocket");

  if (!failures) std::puts("ok");
  std::fflush(stdout);
  std::_Exit(failures ? 1 : 0);  // the kernel32 state is not built for static teardown
}
