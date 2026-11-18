// The analysis result: stems, notes, pitch curves, drum hits, chords, key and tempo.
// Everything the editor shows and the exporters write comes from one Analysis object.
#pragma once

#include "Audio.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ss
{
enum class StemKind : int { Vocals = 0, Drums, Bass, Other, Guitar, Piano, Mix };
enum class Transcribe : int { Off = 0, Mono, Poly, Drums };

const char* stemKindName (StemKind k);
Transcribe defaultTranscribeFor (StemKind k);

struct Note
{
    double start = 0, end = 0;     // seconds from the start of the analysed audio
    int pitch = 60;                // MIDI note (GM drum note on drum stems)
    float velocity = 0.8f;         // 0..1
    std::vector<float> bend;       // optional pitch deviation in semitones, evenly spaced over the note
    bool operator< (const Note& o) const { return start < o.start || (start == o.start && pitch < o.pitch); }
};

// A detected pitch curve (mono stems), one value per hop. 0 = unvoiced.
struct PitchCurve
{
    double hopSeconds = 0.01;
    std::vector<float> midi;       // fractional MIDI pitch, 0 when unvoiced
    std::vector<float> confidence; // 0..1
    float at (double t) const;
};

struct Chord
{
    double start = 0, end = 0;
    int root = -1;                 // pitch class 0..11, -1 = no chord ("N")
    int quality = 0;               // index into chordQualities()
    int bass = -1;                 // pitch class of a slash bass, -1 = root position
    std::string name() const;
    std::vector<int> voicing (int octaveBase = 48) const;   // MIDI notes of a block chord
};

struct ChordQuality { const char* suffix; std::vector<int> intervals; };
const std::vector<ChordQuality>& chordQualities();
bool parseChordName (const std::string& text, Chord& out);   // "F#m7", "C/E", "N"

struct StemResult
{
    StemKind kind = StemKind::Mix;
    std::string name;
    AudioBuffer audio;             // stereo, analysis rate (44.1 kHz)
    Transcribe mode = Transcribe::Off;
    std::vector<Note> notes;
    PitchCurve curve;
};

struct Analysis
{
    uint64_t key = 0;              // hash of the input audio (cache key)
    double sampleRate = 44100.0;
    double duration = 0.0;
    double tempo = 120.0;          // BPM
    double beatOffset = 0.0;       // time of the first downbeat, seconds
    bool tempoFromHost = false;
    int keyRoot = 0;               // 0 = C
    bool keyMinor = false;
    std::string separatorName;
    std::vector<StemResult> stems;
    std::vector<Chord> chords;

    std::string keyName() const;
    int findStem (StemKind k) const;

    // Notes, chords and settings without audio (for the plugin state and the cache).
    std::vector<uint8_t> serialiseData() const;
    bool deserialiseData (const std::vector<uint8_t>& bytes);
};

const char* pitchClassName (int pc);
std::string midiNoteName (int midi);
} // namespace ss
