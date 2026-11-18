#include "runtime/audio/media_player.h"

#include <algorithm>
#include <cmath>

namespace recomp::audio {

namespace {
constexpr int64_t kUnit = 10000000;
float Gain(int32_t v) { return v <= -10000 ? 0.0f : std::pow(10.0f, float(v) / 2000.0f); }
}  // namespace

// The stream ends at the stop position.
class MediaPlayer::Limited : public Source {
 public:
  explicit Limited(std::unique_ptr<Source> in) : in_(std::move(in)), end(in_->Length()) {
    channels_ = in_->channels();
    rate_ = in_->rate();
  }
  int Read(float* out, int n) override {
    int got = in_->Read(out, int(std::clamp<int64_t>(end - pos_, 0, n)));
    pos_ += got;
    return got;
  }
  bool Seek(int64_t f) override {
    pos_ = f;
    return in_->Seek(f);
  }
  int64_t Length() const override { return in_->Length(); }

 private:
  std::unique_ptr<Source> in_;

 public:
  int64_t end;
  int64_t pos_ = 0;
};

MediaPlayer::~MediaPlayer() {
  if (!voice_) return;
  std::lock_guard<std::mutex> l(GetMixer().mu());
  GetMixer().Remove(voice_.get());
}

bool MediaPlayer::Load(Bytes data, std::string* err) {
  auto src = MakeFileSource(std::move(data), err);
  if (!src) return false;
  if (src->Length() < 0) return *err = "unknown duration", false;
  rate_ = src->rate();
  frames_ = src->Length();
  auto lim = std::make_unique<Limited>(std::move(src));
  limit_ = lim.get();
  voice_ = std::make_unique<Voice>();
  voice_->src = std::move(lim);
  voice_->dsound = true;
  voice_->state = Voice::State::Paused;
  std::lock_guard<std::mutex> l(GetMixer().mu());
  voice_->volume = Gain(volume_);
  GetMixer().Add(voice_.get());
  return true;
}

int64_t MediaPlayer::Frame(int64_t t) const { return std::clamp<int64_t>(t * rate_ / kUnit, 0, frames_); }
int64_t MediaPlayer::Time(int64_t frames) const { return frames * kUnit / rate_; }
int64_t MediaPlayer::Duration() const { return Time(frames_); }

int64_t MediaPlayer::StopPosition() {
  std::lock_guard<std::mutex> l(GetMixer().mu());
  return Time(limit_->end);
}

int64_t MediaPlayer::Position() {
  std::lock_guard<std::mutex> l(GetMixer().mu());
  int64_t f = voice_->state == Voice::State::Done ? limit_->end : voice_->Position();
  return Time(std::min(f, limit_->end));
}

void MediaPlayer::SetState(State s) {
  std::lock_guard<std::mutex> l(GetMixer().mu());
  // IMediaControl::Stop keeps the position (Run continues from it): rewinding restarted the pause menu's music.
  voice_->state = s == State::Running ? Voice::State::Playing : Voice::State::Paused;
  state_ = s;
}

void MediaPlayer::SetPositions(uint32_t cur_mode, int64_t* cur, uint32_t stop_mode, int64_t* stop) {
  std::lock_guard<std::mutex> l(GetMixer().mu());
  int64_t end = limit_->end;
  int64_t now = voice_->state == Voice::State::Done ? end : voice_->Position();
  if (stop_mode) {
    end = Frame(stop_mode == 1 ? *stop : Time(end) + *stop);
    limit_->end = end;
  }
  if (cur_mode) {
    const int64_t start = std::min(Frame(cur_mode == 1 ? *cur : Time(now) + *cur), end);
    voice_->Seek(start);
    voice_->passes = 0;
    if (voice_->state == Voice::State::Done)
      voice_->state = state_ == State::Running ? Voice::State::Playing : Voice::State::Paused;
    now = start;
  }
  if (cur) *cur = Time(std::min(now, end));
  if (stop) *stop = Time(end);
}

bool MediaPlayer::SetVolume(int32_t v) {
  if (v > 0 || v < -10000) return false;
  volume_ = v;
  if (voice_) {
    std::lock_guard<std::mutex> l(GetMixer().mu());
    voice_->volume = Gain(v);
  }
  return true;
}

}  // namespace recomp::audio
