#include "Model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace ss
{
const char* stemKindName (StemKind k)
{
    switch (k)
    {
        case StemKind::Vocals: return "Vocals";
        case StemKind::Drums:  return "Drums";
        case StemKind::Bass:   return "Bass";
        case StemKind::Other:  return "Other";
        case StemKind::Guitar: return "Guitar";
        case StemKind::Piano:  return "Piano";
        case StemKind::Mix:    return "Mix";
    }
    return "Stem";
}

Transcribe defaultTranscribeFor (StemKind k)
{
    switch (k)
    {
        case StemKind::Vocals: case StemKind::Bass: return Transcribe::Mono;
        case StemKind::Drums: return Transcribe::Drums;
        default: return Transcribe::Poly;
    }
}

float PitchCurve::at (double t) const
{
    if (midi.empty() || hopSeconds <= 0) return 0.0f;
    const long i = (long) std::lround (t / hopSeconds);
    return i < 0 || i >= (long) midi.size() ? 0.0f : midi[(size_t) i];
}

const char* pitchClassName (int pc)
{
    static const char* n[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    return n[((pc % 12) + 12) % 12];
}

std::string midiNoteName (int midi)
{
    return std::string (pitchClassName (midi)) + std::to_string (midi / 12 - 1);
}

const std::vector<ChordQuality>& chordQualities()
{
    static const std::vector<ChordQuality> q = {
        { "",     { 0, 4, 7 } },
        { "m",    { 0, 3, 7 } },
        { "7",    { 0, 4, 7, 10 } },
        { "maj7", { 0, 4, 7, 11 } },
        { "m7",   { 0, 3, 7, 10 } },
        { "sus2", { 0, 2, 7 } },
        { "sus4", { 0, 5, 7 } },
        { "dim",  { 0, 3, 6 } },
        { "aug",  { 0, 4, 8 } },
        { "add9", { 0, 2, 4, 7 } },
        { "m7b5", { 0, 3, 6, 10 } },
        { "6",    { 0, 4, 7, 9 } },
        { "m6",   { 0, 3, 7, 9 } },
    };
    return q;
}

std::string Chord::name() const
{
    if (root < 0) return "N";
    std::string s = std::string (pitchClassName (root)) + chordQualities()[(size_t) std::clamp (quality, 0, (int) chordQualities().size() - 1)].suffix;
    if (bass >= 0 && bass != root) s += std::string ("/") + pitchClassName (bass);
    return s;
}

std::vector<int> Chord::voicing (int octaveBase) const
{
    std::vector<int> v;
    if (root < 0) return v;
    const auto& q = chordQualities()[(size_t) std::clamp (quality, 0, (int) chordQualities().size() - 1)];
    // root placed between octaveBase and octaveBase+11, upper notes stacked above it
    const int r = octaveBase + ((root - octaveBase % 12) % 12 + 12) % 12;
    if (bass >= 0 && bass != root) v.push_back (r - 12 + (bass - root + 12) % 12);   // slash bass below the chord
    for (int iv : q.intervals) v.push_back (r + iv);
    return v;
}

bool parseChordName (const std::string& text, Chord& out)
{
    std::string t;
    for (char c : text) if (! std::isspace ((unsigned char) c)) t += c;
    if (t.empty() || t == "N" || t == "n" || t == "-") { out.root = -1; out.bass = -1; out.quality = 0; return true; }
    auto pc = [] (const std::string& s, size_t& i) -> int {
        static const int base[] = { 9, 11, 0, 2, 4, 5, 7 };   // A..G
        if (i >= s.size()) return -1;
        const char c = (char) std::toupper ((unsigned char) s[i]);
        if (c < 'A' || c > 'G') return -1;
        int p = base[c - 'A'];
        ++i;
        while (i < s.size() && (s[i] == '#' || s[i] == 'b')) { p += s[i] == '#' ? 1 : -1; ++i; }
        return (p + 12) % 12;
    };
    size_t i = 0;
    const int root = pc (t, i);
    if (root < 0) return false;
    std::string rest = t.substr (i), slash;
    const auto sl = rest.find ('/');
    if (sl != std::string::npos) { slash = rest.substr (sl + 1); rest = rest.substr (0, sl); }
    if (rest == "min" || rest == "-") rest = "m";
    if (rest == "M7" || rest == "Maj7") rest = "maj7";
    int quality = -1;
    for (size_t k = 0; k < chordQualities().size(); ++k) if (rest == chordQualities()[k].suffix) quality = (int) k;
    if (quality < 0) return false;
    int bass = -1;
    if (! slash.empty()) { size_t j = 0; bass = pc (slash, j); if (bass < 0 || j != slash.size()) return false; }
    out.root = root; out.quality = quality; out.bass = bass;
    return true;
}

std::string Analysis::keyName() const
{
    return std::string (pitchClassName (keyRoot)) + (keyMinor ? " minor" : " major");
}

int Analysis::findStem (StemKind k) const
{
    for (size_t i = 0; i < stems.size(); ++i) if (stems[i].kind == k) return (int) i;
    return -1;
}

// ------------------------------------------------------------------------------------------
namespace
{
struct W
{
    std::vector<uint8_t> b;
    template <typename T> void put (T v) { const auto* p = (const uint8_t*) &v; b.insert (b.end(), p, p + sizeof (T)); }
    void str (const std::string& s) { put<uint32_t> ((uint32_t) s.size()); b.insert (b.end(), s.begin(), s.end()); }
    void floats (const std::vector<float>& v) { put<uint32_t> ((uint32_t) v.size()); const auto* p = (const uint8_t*) v.data(); b.insert (b.end(), p, p + v.size() * 4); }
};
struct R
{
    const std::vector<uint8_t>& b; size_t p = 0; bool ok = true;
    template <typename T> T get() { T v {}; if (p + sizeof (T) > b.size()) { ok = false; return v; } std::memcpy (&v, &b[p], sizeof (T)); p += sizeof (T); return v; }
    std::string str() { const auto n = get<uint32_t>(); if (! ok || p + n > b.size()) { ok = false; return {}; } std::string s ((const char*) &b[p], n); p += n; return s; }
    std::vector<float> floats() { const auto n = get<uint32_t>(); if (! ok || p + (size_t) n * 4 > b.size()) { ok = false; return {}; } std::vector<float> v (n); std::memcpy (v.data(), &b[p], (size_t) n * 4); p += (size_t) n * 4; return v; }
};
constexpr uint32_t kMagic = 0x31435353;   // "SSC1"
constexpr uint32_t kVersion = 1;
} // namespace

std::vector<uint8_t> Analysis::serialiseData() const
{
    W w;
    w.put (kMagic); w.put (kVersion);
    w.put (key); w.put (sampleRate); w.put (duration); w.put (tempo); w.put (beatOffset);
    w.put<uint8_t> (tempoFromHost ? 1 : 0); w.put<int32_t> (keyRoot); w.put<uint8_t> (keyMinor ? 1 : 0);
    w.str (separatorName);
    w.put<uint32_t> ((uint32_t) stems.size());
    for (auto& s : stems)
    {
        w.put<int32_t> ((int32_t) s.kind); w.str (s.name); w.put<int32_t> ((int32_t) s.mode);
        w.put<uint32_t> ((uint32_t) s.notes.size());
        for (auto& n : s.notes) { w.put (n.start); w.put (n.end); w.put<int32_t> (n.pitch); w.put (n.velocity); w.floats (n.bend); }
        w.put (s.curve.hopSeconds); w.floats (s.curve.midi); w.floats (s.curve.confidence);
    }
    w.put<uint32_t> ((uint32_t) chords.size());
    for (auto& c : chords) { w.put (c.start); w.put (c.end); w.put<int32_t> (c.root); w.put<int32_t> (c.quality); w.put<int32_t> (c.bass); }
    return w.b;
}

bool Analysis::deserialiseData (const std::vector<uint8_t>& bytes)
{
    R r { bytes };
    if (r.get<uint32_t>() != kMagic || r.get<uint32_t>() > kVersion) return false;
    Analysis a;
    a.key = r.get<uint64_t>(); a.sampleRate = r.get<double>(); a.duration = r.get<double>();
    a.tempo = r.get<double>(); a.beatOffset = r.get<double>();
    a.tempoFromHost = r.get<uint8_t>() != 0; a.keyRoot = r.get<int32_t>(); a.keyMinor = r.get<uint8_t>() != 0;
    a.separatorName = r.str();
    const auto ns = r.get<uint32_t>();
    for (uint32_t i = 0; i < ns && r.ok; ++i)
    {
        StemResult s;
        s.kind = (StemKind) r.get<int32_t>(); s.name = r.str(); s.mode = (Transcribe) r.get<int32_t>();
        const auto nn = r.get<uint32_t>();
        for (uint32_t k = 0; k < nn && r.ok; ++k)
        {
            Note n;
            n.start = r.get<double>(); n.end = r.get<double>(); n.pitch = r.get<int32_t>(); n.velocity = r.get<float>(); n.bend = r.floats();
            s.notes.push_back (std::move (n));
        }
        s.curve.hopSeconds = r.get<double>(); s.curve.midi = r.floats(); s.curve.confidence = r.floats();
        a.stems.push_back (std::move (s));
    }
    const auto nc = r.get<uint32_t>();
    for (uint32_t i = 0; i < nc && r.ok; ++i)
    {
        Chord c;
        c.start = r.get<double>(); c.end = r.get<double>(); c.root = r.get<int32_t>(); c.quality = r.get<int32_t>(); c.bass = r.get<int32_t>();
        a.chords.push_back (c);
    }
    if (! r.ok) return false;
    // keep the audio we already hold when the stems line up
    if (a.stems.size() == stems.size())
        for (size_t i = 0; i < stems.size(); ++i)
            if (a.stems[i].kind == stems[i].kind) a.stems[i].audio = std::move (stems[i].audio);
    *this = std::move (a);
    return true;
}
} // namespace ss
