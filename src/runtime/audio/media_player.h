#pragma once
// One decoded audio file on a voice of the software mixer, with the
// transport of a DirectShow filter graph: stopped / paused / running, a
// start and a stop position in 100 ns media time, and a volume in
// hundredths of a dB. A stream that reaches its stop position stays put
// there (position == stop) until it is positioned again.

#include <cstdint>
#include <memory>
#include <string>

#include "runtime/audio/mixer.h"
#include "runtime/audio/source.h"

namespace recomp::audio {

class MediaPlayer {
 public:
  enum class State { Stopped, Paused, Running };

  MediaPlayer() = default;
  ~MediaPlayer();
  MediaPlayer(const MediaPlayer&) = delete;
  MediaPlayer& operator=(const MediaPlayer&) = delete;

  // Opens a file image; false and *err if it has no audio with a known
  // length. The player is then Stopped at 0 with the stop at the end.
  bool Load(Bytes data, std::string* err);
  bool loaded() const { return voice_ != nullptr; }

  State state() const { return state_; }
  void Run() { SetState(State::Running); }
  void Pause() { SetState(State::Paused); }
  // Rewinds to the start position set last.
  void Stop() { SetState(State::Stopped); }

  int64_t Duration() const;
  int64_t Position();
  int64_t StopPosition();

  // AM_SEEKING_PositioningBitMask modes: 0 leave, 1 absolute, 2 relative
  // to the current value, 3 incremental. For modes other than 0 *cur and
  // *stop are read; all that are non-null are written back with the
  // positions reached.
  void SetPositions(uint32_t cur_mode, int64_t* cur, uint32_t stop_mode, int64_t* stop);

  // IBasicAudio: -10000 (silence) to 0.
  bool SetVolume(int32_t hundredths_db);
  int32_t volume() const { return volume_; }

 private:
  class Limited;
  void SetState(State s);
  int64_t Frame(int64_t t) const;
  int64_t Time(int64_t frames) const;

  std::unique_ptr<Voice> voice_;
  Limited* limit_ = nullptr;
  int64_t frames_ = 0;
  int rate_ = 0;
  int32_t volume_ = 0;
  State state_ = State::Stopped;
};

}  // namespace recomp::audio
