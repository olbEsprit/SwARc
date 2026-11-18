// MediaPlayer (runtime/audio/media_player.h) through the offline mixer.
// MEDIA_PLAYER_TEST_FILE: also play that file (any container libavformat
// reads); nothing of it is in the source tree.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "runtime/audio/media_player.h"

using namespace recomp::audio;

namespace {

int g_fail = 0;
#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) {                                                     \
      std::printf("FAIL line %d: %s\n", __LINE__, #c);              \
      g_fail++;                                                     \
    }                                                               \
  } while (0)

constexpr int kRate = 48000;
constexpr int64_t kSec = 10000000;

Bytes SineWav(int rate, double seconds) {
  int frames = int(rate * seconds), ch = 2;
  auto v = std::make_shared<std::vector<uint8_t>>();
  auto put32 = [&](uint32_t x) { for (int i = 0; i < 4; i++) v->push_back(uint8_t(x >> (8 * i))); };
  auto put16 = [&](uint16_t x) { v->push_back(uint8_t(x)); v->push_back(uint8_t(x >> 8)); };
  uint32_t data = uint32_t(frames * ch * 2);
  v->insert(v->end(), {'R', 'I', 'F', 'F'});
  put32(36 + data);
  v->insert(v->end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(16); put16(1); put16(ch); put32(rate); put32(rate * ch * 2); put16(ch * 2); put16(16);
  v->insert(v->end(), {'d', 'a', 't', 'a'});
  put32(data);
  for (int i = 0; i < frames; i++) {
    int16_t s = int16_t(std::lrint(16000 * std::sin(2 * M_PI * 440.0 * i / rate)));
    put16(uint16_t(s));
    put16(uint16_t(s));
  }
  return v;
}

// Mixes `seconds` and returns the RMS of the output.
double Render(double seconds) {
  std::vector<float> out(2 * 480);
  double sum = 0;
  int64_t n = 0;
  for (int i = 0; i < int(seconds * 100); i++) {
    GetMixer().Mix(out.data(), 480);
    for (float f : out) sum += double(f) * f;
    n += int64_t(out.size());
  }
  return std::sqrt(sum / double(n));
}

void TestSource() {
  std::string err;
  auto src = MakeFileSource(SineWav(22050, 2.0), &err);
  CHECK(src);
  if (!src) return;
  CHECK(src->Length() == 44100);
  CHECK(src->rate() == 22050 && src->channels() == 2);
  std::vector<float> all(44100 * 2), part(2000 * 2);
  CHECK(src->Read(all.data(), 44100) == 44100);
  CHECK(src->Read(part.data(), 10) == 0);
  CHECK(src->Seek(30000));
  CHECK(src->Read(part.data(), 2000) == 2000);
  CHECK(std::memcmp(part.data(), all.data() + 30000 * 2, sizeof(float) * part.size()) == 0);
  CHECK(src->Seek(0));
  CHECK(src->Read(part.data(), 2000) == 2000);
  CHECK(std::memcmp(part.data(), all.data(), sizeof(float) * part.size()) == 0);
  double e = 0;
  for (float f : all) e += double(f) * f;
  CHECK(e > 1000);
  auto bad = std::make_shared<std::vector<uint8_t>>(100, 7);
  CHECK(!MakeFileSource(bad, &err));
}

void TestPlayer(Bytes data, double seconds, bool exact_seek) {
  GetMixer().OpenOffline(kRate);
  MediaPlayer p;
  std::string err;
  CHECK(!p.loaded());
  CHECK(p.Load(data, &err));
  CHECK(p.loaded());
  CHECK(std::llabs(p.Duration() - int64_t(seconds * kSec)) < kSec / 20);
  CHECK(p.state() == MediaPlayer::State::Stopped);
  CHECK(p.Position() == 0 && p.StopPosition() == p.Duration());
  CHECK(Render(0.2) == 0);

  p.Run();
  CHECK(p.state() == MediaPlayer::State::Running);
  double full = Render(0.5);
  CHECK(full > 0.01);
  int64_t pos = p.Position();
  CHECK(std::llabs(pos - kSec / 2) < kSec / 20);
  p.Pause();
  CHECK(p.state() == MediaPlayer::State::Paused);
  CHECK(Render(0.2) == 0);
  CHECK(p.Position() == pos);
  p.Stop();
  CHECK(p.state() == MediaPlayer::State::Stopped && p.Position() == pos);

  int64_t cur = 0, stop = 0;
  p.SetPositions(1, &cur, 0, &stop);
  CHECK(stop == p.Duration());
  p.Run();
  double a = Render(0.5);
  CHECK(a > 0.01);
  CHECK(!p.SetVolume(1) && !p.SetVolume(-10001));
  CHECK(p.SetVolume(-2000) && p.volume() == -2000);
  cur = 0;
  p.SetPositions(1, &cur, 0, &stop);
  Render(0.05);
  cur = 0;
  p.SetPositions(1, &cur, 0, &stop);
  double tenth = Render(0.5);
  std::printf("level at -20 dB / level at 0 dB: %.4f\n", tenth / a);
  if (exact_seek) CHECK(std::fabs(tenth / a - 0.1) < 0.01);
  else CHECK(tenth > 0.05 * a && tenth < 0.2 * a);
  CHECK(p.SetVolume(-10000));
  Render(0.05);
  cur = 0;
  p.SetPositions(1, &cur, 0, &stop);
  CHECK(Render(0.3) == 0);
  CHECK(p.SetVolume(0));

  cur = kSec / 2;
  stop = kSec;
  p.SetPositions(1, &cur, 1, &stop);
  CHECK(cur == kSec / 2 && stop == kSec);
  Render(1.0);
  CHECK(p.Position() == kSec && p.StopPosition() == kSec);
  CHECK(Render(0.2) == 0);
  CHECK(p.state() == MediaPlayer::State::Running);
  CHECK(p.Position() == kSec);

  cur = kSec / 4;
  p.SetPositions(1, &cur, 0, &stop);
  CHECK(std::llabs(cur - kSec / 4) < kSec / 1000 && stop == kSec);
  CHECK(Render(0.2) > 0.01);
  CHECK(p.Position() < kSec);
  int64_t before = p.Position();
  cur = kSec / 10;
  p.SetPositions(3, &cur, 0, &stop);
  CHECK(std::llabs(cur - before - kSec / 10) < kSec / 1000);
  int64_t at = p.Position();
  p.Stop();
  CHECK(p.Position() == at);
}

}  // namespace

int main() {
  TestSource();
  TestPlayer(SineWav(22050, 2.0), 2.0, true);
  if (const char* f = std::getenv("MEDIA_PLAYER_TEST_FILE")) {
    FILE* fp = std::fopen(f, "rb");
    if (!fp) {
      std::printf("FAIL: cannot open %s\n", f);
      return 1;
    }
    auto v = std::make_shared<std::vector<uint8_t>>();
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) v->insert(v->end(), buf, buf + n);
    std::fclose(fp);
    std::string err;
    auto src = MakeFileSource(v, &err);
    CHECK(src);
    if (src) {
      double secs = double(src->Length()) / src->rate();
      std::printf("runtime file: %.2f s, %d Hz, %d ch\n", secs, src->rate(), src->channels());
      int ch = src->channels();
      int64_t at = src->Length() / 3;
      std::vector<float> seq(size_t(at + 8000) * ch), seek(8000 * ch);
      CHECK(src->Read(seq.data(), int(at + 8000)) == at + 8000);
      CHECK(src->Seek(at));
      CHECK(src->Read(seek.data(), 8000) == 8000);
      double worst = 0;
      for (int i = 0; i < 8000 * ch; i++)
        worst = std::max(worst, double(std::fabs(seek[i] - seq[size_t(at) * ch + i])));
      std::printf("max difference after a seek: %.6f\n", worst);
      CHECK(worst == 0);
      src.reset();
      TestPlayer(v, secs, false);
    }
  } else {
    std::printf("MEDIA_PLAYER_TEST_FILE not set: runtime file skipped\n");
  }
  std::printf(g_fail ? "FAILED\n" : "OK\n");
  return g_fail ? 1 : 0;
}
