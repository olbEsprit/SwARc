# DirectShow (quartz) as HLE

`CLSID_FilterGraph` is a graph holding one audio file: `IGraphBuilder`,
`IMediaControl`, `IMediaSeeking`, `IBasicAudio`, plus the `IBaseFilter` and
`IPin` that `AddSourceFilter` and `FindPin` hand out. The file is decoded by
libavformat (`audio::FileSource`, any container, e.g. ASF/WMA) and plays on a
voice of the software mixer (`audio::MediaPlayer`). Other methods have no
handler and are fatal when called.

Observed use, as a player of streamed music and speech makes it:

- `AddSourceFilter(path, path, &filter)`, `filter->FindPin(L"Output")`,
  `IMediaControl::Stop`, `IGraphBuilder::Render(pin)`; with no media in the
  graph yet Run, Pause and Stop succeed.
- `SetTimeFormat(TIME_FORMAT_MEDIA_TIME)`, then `SetPositions(&0, absolute,
  &0, none)` to rewind.
- Play: `put_Volume`, `SetPositions(&start, absolute | ReturnTime, &stop,
  absolute)`, `Run`. The stop position ends the stream there.
- Progress is polled with `GetPositions(&cur, &stop)`; the stream counts as
  finished when `cur >= stop`, so a stream that has played to its stop reports
  `cur == stop` and stays Running until it is stopped or positioned again.
- Volume is hundredths of a dB, -10000 to 0 (`10^(v/2000)` linear, silence at
  -10000).

Seeking decodes up to the target (containers land on packet boundaries that
do not pin the decoder's frames), so positions are exact; a stream of a few
minutes costs well under 0.1 s.

`<ID>_AUDIO_RMS` logs the mixer's output level (RMS of the final mix) once per
second of mixed audio.
