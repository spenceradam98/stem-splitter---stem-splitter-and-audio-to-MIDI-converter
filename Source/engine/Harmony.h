// Tempo / beat grid, key and chord detection. All our own code (no third-party models).
#pragma once

#include "Model.h"

#include <vector>

namespace ss
{
struct TempoEstimate { double bpm = 120.0; double offset = 0.0; float confidence = 0.0f; };

// Onset-autocorrelation tempo (60..200 BPM, prior around 120) and the phase of the beat grid.
TempoEstimate estimateTempo (const std::vector<float>& mono, double sampleRate);

// Krumhansl-Schmuckler key from duration-weighted pitch classes of the given notes.
void detectKey (const std::vector<const std::vector<Note>*>& noteSets, int& root, bool& minor);

struct ChordSettings
{
    double beatsPerFrame = 1.0;    // chord resolution on the beat grid
    float changePenalty = 0.12f;   // larger = fewer chord changes
    bool slashChords = true;
};

// Chords from polyphonic notes (pads/keys/guitar) plus the bass line (roots and slash basses),
// on the beat grid given by tempo and offset. Viterbi smoothing stops chords from flickering.
std::vector<Chord> detectChords (const std::vector<const std::vector<Note>*>& harmonic, const std::vector<Note>* bass,
                                 double duration, double bpm, double offset, const ChordSettings& s = {});
} // namespace ss
