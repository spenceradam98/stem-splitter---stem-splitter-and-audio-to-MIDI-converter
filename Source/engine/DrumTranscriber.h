// Drum transcription (v1): onset detection per frequency band on the drum stem, then a small
// hand-tuned classifier (band energies, spectral centroid, decay) sorts each hit into kick, snare,
// closed hat or open hat. Hits are returned as short notes on the General MIDI drum map, the same
// map drum-foundry uses.
#pragma once

#include "Model.h"

#include <vector>

namespace ss
{
namespace gm
{
constexpr int kick = 36, snare = 38, closedHat = 42, openHat = 46;
}

struct DrumSettings
{
    float sensitivity = 1.0f;   // > 1 finds quieter hits
    double hitLength = 0.06;    // length of the exported notes, seconds
};

std::vector<Note> transcribeDrums (const std::vector<float>& mono, double sampleRate, const DrumSettings& s = {});
const char* gmDrumName (int note);
} // namespace ss
