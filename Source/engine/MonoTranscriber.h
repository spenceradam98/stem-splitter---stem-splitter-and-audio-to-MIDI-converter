// Monophonic transcription for bass and vocal stems: a pYIN-style pitch tracker (YIN with a
// distribution of thresholds and an HMM over pitch bins and voicing) followed by note
// segmentation that keeps vibrato and bends as per-note pitch-bend curves.
#pragma once

#include "Model.h"

#include <atomic>
#include <vector>

namespace ss
{
struct MonoSettings
{
    float minHz = 30.0f, maxHz = 400.0f;
    float minNoteSeconds = 0.06f;
    float gateDb = -45.0f;          // frames quieter than this (relative to the stem's loudest frame) are unvoiced
    float splitSemitones = 0.6f;    // a held pitch change this large starts a new note
    float splitHoldSeconds = 0.05f; // ...when it lasts this long
    float scoopSeconds = 0.12f;     // shorter pieces glued to the next note (scoops, slides into notes)
    bool splitOnOnsets = true;      // re-articulated notes at the same pitch (bass plucks)
    bool keepBends = true;          // store vibrato / bends as pitch-bend curves

    static MonoSettings bass();
    static MonoSettings vocals();
};

PitchCurve trackPitch (const std::vector<float>& mono, double sampleRate, const MonoSettings& s,
                       const std::atomic<bool>* cancel = nullptr);

std::vector<Note> segmentNotes (const PitchCurve& curve, const std::vector<float>& mono, double sampleRate,
                                const MonoSettings& s);
} // namespace ss
