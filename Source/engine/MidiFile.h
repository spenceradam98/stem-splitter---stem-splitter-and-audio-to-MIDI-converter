// Standard MIDI File writer (type 1) for stems, chord tracks and bass-root tracks, plus note
// post-processing (quantize, transpose, velocity scaling) used by the editor and the exporters.
#pragma once

#include "Model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ss
{
struct MidiTrackSpec
{
    std::string name;
    int channel = 0;                    // 0..15 (9 = GM drums)
    int program = -1;                   // GM program change, -1 = none
    std::vector<Note> notes;
    bool pitchBends = true;
};

struct MidiFileOptions
{
    double bpm = 120.0;
    double startOffset = 0.0;           // seconds subtracted from every time (e.g. capture start)
    int ppq = 480;
    int bendRangeSemitones = 12;
};

std::vector<uint8_t> writeMidiFile (const std::vector<MidiTrackSpec>& tracks, const MidiFileOptions& o);

// Convenience: the MIDI of one stem (drums on channel 10), or of every transcribed stem plus chords.
MidiTrackSpec trackForStem (const StemResult& s);
MidiTrackSpec chordTrack (const std::vector<Chord>& chords, bool rootsOnly = false);
std::vector<uint8_t> exportStemMidi (const Analysis& a, int stemIndex);
std::vector<uint8_t> exportChordMidi (const Analysis& a, bool rootsOnly = false);
std::vector<uint8_t> exportAllMidi (const Analysis& a, bool withChords = true);

// Moves note starts (and ends, keeping length) towards a grid of `division` steps per beat.
void quantizeNotes (std::vector<Note>& notes, double bpm, double offset, int division, float strength);
void transposeNotes (std::vector<Note>& notes, int semitones);
void scaleVelocities (std::vector<Note>& notes, float amount);   // 0 = flat 0.8, 1 = unchanged, 2 = exaggerated

// Minimal reader used by the tests: returns note-on/off pairs per channel as Notes.
bool readMidiNotes (const std::vector<uint8_t>& bytes, std::vector<std::vector<Note>>& tracks, double& bpm);
} // namespace ss
