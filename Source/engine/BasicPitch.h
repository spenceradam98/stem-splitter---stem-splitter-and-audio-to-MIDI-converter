// Polyphonic transcription with Spotify's Basic Pitch model (Apache 2.0, ICASSP 2022 weights).
// The network runs through GraphRunner, so no ML runtime is needed. Windowing, output unwrapping and
// note creation follow basic_pitch/inference.py and note_creation.py (v0.4.0).
#pragma once

#include "GraphRunner.h"
#include "Model.h"

#include <atomic>
#include <functional>
#include <vector>

namespace ss
{
struct PolySettings
{
    float onsetThreshold = 0.5f;
    float frameThreshold = 0.3f;
    float minNoteMs = 127.7f;
    float minHz = 0.0f, maxHz = 0.0f;   // 0 = unlimited
    bool melodiaTrick = true;
    bool inferOnsets = true;
    bool pitchBends = true;
};

class BasicPitch
{
public:
    static constexpr int kSampleRate = 22050;
    static constexpr int kHop = 256;
    static constexpr int kWindow = 43844;          // AUDIO_N_SAMPLES
    static constexpr int kFramesPerWindow = 172;   // ANNOT_N_FRAMES
    static constexpr int kOverlapFrames = 30;
    static constexpr int kMidiOffset = 21;

    struct Output
    {
        std::vector<std::vector<float>> note, onset, contour;   // frames x 88 / 88 / 264
        size_t frames() const { return note.size(); }
    };

    bool loadEmbedded();                       // the model compiled into the engine
    bool load (const void* data, size_t size);
    bool isLoaded() const { return graph.isLoaded(); }

    // mono audio at any rate. threads <= 0 picks the hardware count.
    Output infer (const std::vector<float>& mono, double sampleRate, int threads = 0,
                  std::function<void (float)> progress = {}, const std::atomic<bool>* cancel = nullptr) const;

    static std::vector<Note> toNotes (Output out, const PolySettings& s = {});
    static double frameTime (size_t frame);

private:
    GraphRunner graph;
};
} // namespace ss
