# stem-scribe design notes

## Phase 0 results (feasibility spike)

**Separation engine: HTDemucs 4-stem through demucs.cpp, not ONNX Runtime.**

- demucs.cpp (MIT, pinned at `4edae49`) is plain C++17 plus header-only Eigen. It has no runtime library
  to ship, no DLL inside the .vst3, and no lipo'd universal binaries to assemble. It builds for macOS 11
  universal and for any x86-64 without AVX2 once its `-march=native` flag is left out (we compile its
  sources ourselves for that reason). Eigen is used in `EIGEN_MPL2_ONLY` mode.
- ONNX Runtime would add a prebuilt library per platform, and recent macOS releases of it target newer
  macOS than our macOS 11 floor, so it fails the "runs on Big Sur Intel" rule without building ORT from
  source. HTDemucs also needs custom STFT handling around the ONNX graph.
- Weights: the official Demucs v4 `htdemucs` weights (MIT), converted to demucs.cpp's ggml f16 format,
  about 81 MB. The installers come out around 100 MB, inside the agreed 150 to 300 MB budget.
- Speed: the sandbox used for development could not download the weights, so separation speed on real
  music is measured by the CI `test` job (a 60 s clip through `stemscribe_cli`, printed with `time`) and
  by the engine test that runs when `STEMSCRIBE_TEST_MODEL` is set. demucs.cpp's own published numbers put
  a 4-minute song at around a minute on a recent desktop CPU and several minutes on an old Intel laptop,
  which matches the plan. The separator runs sections on several threads with 0.75 s overlaps and
  linear cross-fades. Results are cached, so a song is only split once.
- Transcription without splitting is fast: a 4-minute synthetic song takes 14 s on 2 cores
  (17x realtime), most of it Basic Pitch.

**Basic Pitch without an ML runtime.** The Basic Pitch ONNX model (Apache 2.0) is simplified to a fixed
input of 43,844 samples and exported by `tools/export_basic_pitch.py` to a small binary graph
(`Source/engine/models/basic_pitch.bpg`, 191 KB), which CMake compiles into the engine. `GraphRunner`
interprets it (Conv, Pad, Reshape, Transpose, Slice, Concat, element-wise ops, reductions). Its outputs
were checked against the reference model during the spike.

**Licences checked:** demucs.cpp MIT, Demucs code and weights MIT, Eigen MPL 2.0, Basic Pitch Apache 2.0,
JUCE 8 (AGPLv3 or a commercial licence; a JUCE licence is needed before closed-source release).
Nothing with unclear terms is bundled. See `THIRD_PARTY_NOTICES.md`.

## Engine (`Source/engine`, no JUCE)

- `Separator`: interface plus the demucs.cpp implementation. Stems come back as drums, bass, other, vocals.
  The model is loaded lazily on the first split and kept.
- `MonoTranscriber` (bass, vocals): pYIN-style tracker. YIN computed via FFT, a Beta(2,18) prior over
  thresholds, and a Viterbi over 25-cent pitch bins times voiced/unvoiced. Notes are cut where the pitch
  holds steady, at onsets, and scoops into a note are merged into it; the remaining movement becomes
  pitch bend (range 12 semitones, sent by RPN). The pitch curve is kept for the roll.
- `BasicPitch` (other, and the mix when not splitting): polyphonic notes from the onset and note maps.
- `DrumTranscriber`: band-split onset detection plus a hand-tuned classifier for kick, snare, closed
  and open hat (GM 36, 38, 42, 46, so it plays drum-foundry directly).
- `Harmony`: tempo from onset autocorrelation; chroma per beat matched against 13 chord qualities with
  a Viterbi and a change penalty, slash basses, and Krumhansl key detection (relative major wins a near tie).
- `MidiFile`: type 1 SMF, one track and channel per stem, drums on channel 10, plus a block-chord track.
- `Pipeline`: runs the stages with progress and cancel, and keys the cache by a hash of the audio.

## Plugin (`Source/plugin`)

- An effect with MIDI out, so it can capture the track it sits on. Capture records along the host
  timeline (up to 10 minutes) and remembers where it started, so "Place at host playhead" lines the
  MIDI up.
- Analysis runs on a worker thread. Each job returns a function applied on the message thread. The audio
  thread only try-locks around the preview audio and the MIDI event list, so it never waits.
- Stem list: solo, mute, level, live MIDI out per stem, transcription mode (re-runs that stem), and drag
  chips for WAV and .mid (plus "All MIDI").
- Roll: ruler, chord lane (double-click to rename, right-click for options), waveform strip, piano roll
  with the pitch curve over the notes, faint notes from other stems, and a drum-lane grid for drums.
  Editing: move, resize, Alt-drag velocity, rubber band, draw, delete, split (S), merge (M), quantize (Q),
  transpose (arrows), grid and scale snap, undo and redo.
- State saves the file reference, the analysis (compressed) and the edits with the DAW session.
- Five themes, each drawing notes differently: Studio Night, Paper Score, Neon Grid, Spectral (spectrogram
  behind the roll) and Tape Deck.

## Choices made where the plan left questions open

- 4-stem HTDemucs only for Phase 1; the 6-stem model and GPU acceleration stay in Phase 2.
- fp16 weights, bundled in the installer (offline, no download server).
- Company name and bundle ids use the placeholder "YourBrand" until a brand is chosen.
- Live MIDI out was cheap to add, so it is in now rather than Phase 3.

## Known risks

- The MSVC build of demucs.cpp has not been compiled yet; the first Windows CI run is the test.
- Separation speed on 2015-era Intel Macs (several minutes per song) is expected but unmeasured.
- Dense mixes give imperfect MIDI, which is what the editor is for.
