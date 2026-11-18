#include "MidiFile.h"

#include "DrumTranscriber.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace ss
{
namespace
{
struct Ev { int64_t tick; int order; std::vector<uint8_t> bytes; };

void varLen (std::vector<uint8_t>& v, uint32_t x)
{
    uint8_t buf[5]; int n = 0;
    buf[n++] = (uint8_t) (x & 0x7f);
    while (x >>= 7) buf[n++] = (uint8_t) (0x80 | (x & 0x7f));
    while (n) v.push_back (buf[--n]);
}
void be32 (std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back ((uint8_t) (x >> s)); }
void be16 (std::vector<uint8_t>& v, uint16_t x) { v.push_back ((uint8_t) (x >> 8)); v.push_back ((uint8_t) x); }

std::vector<uint8_t> chunk (const std::vector<Ev>& evsIn)
{
    auto evs = evsIn;
    std::stable_sort (evs.begin(), evs.end(), [] (const Ev& a, const Ev& b) { return a.tick != b.tick ? a.tick < b.tick : a.order < b.order; });
    std::vector<uint8_t> d;
    int64_t last = 0;
    for (auto& e : evs)
    {
        varLen (d, (uint32_t) std::max<int64_t> (0, e.tick - last));
        last = std::max (last, e.tick);
        d.insert (d.end(), e.bytes.begin(), e.bytes.end());
    }
    d.insert (d.end(), { 0x00, 0xff, 0x2f, 0x00 });
    std::vector<uint8_t> c { 'M', 'T', 'r', 'k' };
    be32 (c, (uint32_t) d.size());
    c.insert (c.end(), d.begin(), d.end());
    return c;
}

Ev meta (int64_t t, uint8_t type, const std::string& s)
{
    Ev e { t, 0, { 0xff, type } };
    varLen (e.bytes, (uint32_t) s.size());
    e.bytes.insert (e.bytes.end(), s.begin(), s.end());
    return e;
}
} // namespace

std::vector<uint8_t> writeMidiFile (const std::vector<MidiTrackSpec>& tracks, const MidiFileOptions& o)
{
    const double ticksPerSec = o.bpm / 60.0 * o.ppq;
    auto tick = [&] (double sec) { return (int64_t) std::llround (std::max (0.0, sec - o.startOffset) * ticksPerSec); };
    std::vector<uint8_t> f { 'M', 'T', 'h', 'd' };
    be32 (f, 6); be16 (f, 1); be16 (f, (uint16_t) (tracks.size() + 1)); be16 (f, (uint16_t) o.ppq);

    // conductor track: tempo + 4/4
    std::vector<Ev> cond;
    cond.push_back (meta (0, 0x03, "stem-scribe"));
    const uint32_t uspq = (uint32_t) std::lround (60000000.0 / std::max (1.0, o.bpm));
    cond.push_back ({ 0, 1, { 0xff, 0x51, 0x03, (uint8_t) (uspq >> 16), (uint8_t) (uspq >> 8), (uint8_t) uspq } });
    cond.push_back ({ 0, 2, { 0xff, 0x58, 0x04, 4, 2, 24, 8 } });
    auto c = chunk (cond);
    f.insert (f.end(), c.begin(), c.end());

    for (auto& tr : tracks)
    {
        std::vector<Ev> ev;
        const uint8_t ch = (uint8_t) std::clamp (tr.channel, 0, 15);
        ev.push_back (meta (0, 0x03, tr.name));
        if (tr.program >= 0) ev.push_back ({ 0, 1, { (uint8_t) (0xC0 | ch), (uint8_t) std::clamp (tr.program, 0, 127) } });
        // which notes may carry bends: only those that do not overlap another note on the track
        std::vector<Note> notes = tr.notes;
        std::sort (notes.begin(), notes.end());
        std::vector<bool> bendOk (notes.size(), false);
        bool anyBend = false;
        for (size_t i = 0; i < notes.size(); ++i)
        {
            if (! tr.pitchBends || notes[i].bend.empty() || ch == 9) continue;
            bool overl = (i > 0 && notes[i - 1].end > notes[i].start + 1e-4) || (i + 1 < notes.size() && notes[i + 1].start < notes[i].end - 1e-4);
            for (size_t j = 0; j < i && ! overl; ++j) overl = notes[j].end > notes[i].start + 1e-4;
            bendOk[i] = ! overl;
            anyBend = anyBend || bendOk[i];
        }
        if (anyBend)
        {
            // RPN 0 (pitch-bend sensitivity) = bendRangeSemitones
            const uint8_t cc = (uint8_t) (0xB0 | ch);
            ev.push_back ({ 0, 2, { cc, 101, 0, 0, cc, 100, 0, 0, cc, 6, (uint8_t) std::clamp (o.bendRangeSemitones, 1, 24), 0, cc, 38, 0, 0, cc, 101, 127, 0, cc, 100, 127 } });
        }
        auto bendEv = [&] (int64_t t, float semis, int order) {
            const int v = std::clamp ((int) std::lround (8192.0 + semis / o.bendRangeSemitones * 8191.0), 0, 16383);
            ev.push_back ({ t, order, { (uint8_t) (0xE0 | ch), (uint8_t) (v & 0x7f), (uint8_t) (v >> 7) } });
        };
        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto& n = notes[i];
            const int64_t a = tick (n.start), b = std::max (a + 1, tick (n.end));
            const uint8_t key = (uint8_t) std::clamp (n.pitch, 0, 127);
            const uint8_t vel = (uint8_t) std::clamp ((int) std::lround (n.velocity * 127.0f), 1, 127);
            if (bendOk[i])
            {
                float lastV = 1e9f;
                for (size_t k = 0; k < n.bend.size(); ++k)
                {
                    const float v = n.bend[k];
                    if (std::abs (v - lastV) < 1.0f / 64.0f) continue;
                    lastV = v;
                    const int64_t t = a + (int64_t) std::llround ((double) (b - a) * (double) k / (double) n.bend.size());
                    bendEv (t, v, k == 0 ? 3 : 5);
                }
            }
            ev.push_back ({ a, 4, { (uint8_t) (0x90 | ch), key, vel } });
            ev.push_back ({ b, 0, { (uint8_t) (0x80 | ch), key, 0 } });
            if (bendOk[i]) bendEv (b, 0.0f, 1);
        }
        auto t = chunk (ev);
        f.insert (f.end(), t.begin(), t.end());
    }
    return f;
}

MidiTrackSpec trackForStem (const StemResult& s)
{
    MidiTrackSpec t;
    t.name = s.name.empty() ? stemKindName (s.kind) : s.name;
    t.notes = s.notes;
    switch (s.kind)
    {
        case StemKind::Drums:  t.channel = 9; break;
        case StemKind::Bass:   t.channel = 1; t.program = 33; break;   // fingered bass
        case StemKind::Vocals: t.channel = 2; t.program = 53; break;   // voice oohs
        case StemKind::Guitar: t.channel = 4; t.program = 25; break;
        case StemKind::Piano:  t.channel = 5; t.program = 0; break;
        default:               t.channel = 3; t.program = 89; break;   // warm pad
    }
    if (s.mode == Transcribe::Drums) t.channel = 9;
    return t;
}

MidiTrackSpec chordTrack (const std::vector<Chord>& chords, bool rootsOnly)
{
    MidiTrackSpec t;
    t.name = rootsOnly ? "Chord roots" : "Chords";
    t.channel = rootsOnly ? 6 : 7;
    t.program = rootsOnly ? 38 : 0;
    t.pitchBends = false;
    for (auto& c : chords)
    {
        if (c.root < 0) continue;
        std::vector<int> keys;
        if (rootsOnly) keys.push_back (36 + (c.bass >= 0 ? c.bass : c.root));
        else keys = c.voicing (48);
        for (int k : keys) { Note n; n.start = c.start; n.end = c.end; n.pitch = k; n.velocity = 0.7f; t.notes.push_back (n); }
    }
    return t;
}

std::vector<uint8_t> exportStemMidi (const Analysis& a, int i)
{
    MidiFileOptions o; o.bpm = a.tempo;
    if (i < 0 || i >= (int) a.stems.size()) return writeMidiFile ({}, o);
    return writeMidiFile ({ trackForStem (a.stems[(size_t) i]) }, o);
}

std::vector<uint8_t> exportChordMidi (const Analysis& a, bool rootsOnly)
{
    MidiFileOptions o; o.bpm = a.tempo;
    return writeMidiFile ({ chordTrack (a.chords, rootsOnly) }, o);
}

std::vector<uint8_t> exportAllMidi (const Analysis& a, bool withChords)
{
    MidiFileOptions o; o.bpm = a.tempo;
    std::vector<MidiTrackSpec> t;
    for (auto& s : a.stems) if (s.mode != Transcribe::Off && ! s.notes.empty()) t.push_back (trackForStem (s));
    if (withChords && ! a.chords.empty()) { t.push_back (chordTrack (a.chords)); t.push_back (chordTrack (a.chords, true)); }
    return writeMidiFile (t, o);
}

void quantizeNotes (std::vector<Note>& notes, double bpm, double offset, int division, float strength)
{
    if (bpm <= 0 || division <= 0) return;
    const double grid = 60.0 / bpm / division;
    strength = std::clamp (strength, 0.0f, 1.0f);
    for (auto& n : notes)
    {
        const double len = n.end - n.start;
        const double q = offset + std::round ((n.start - offset) / grid) * grid;
        n.start += (q - n.start) * strength;
        double qlen = std::max (grid, std::round (len / grid) * grid);
        n.end = n.start + len + (qlen - len) * strength;
        if (n.start < 0) { n.end -= n.start; n.start = 0; }
    }
    std::sort (notes.begin(), notes.end());
}

void transposeNotes (std::vector<Note>& notes, int semis)
{
    for (auto& n : notes) n.pitch = std::clamp (n.pitch + semis, 0, 127);
}

void scaleVelocities (std::vector<Note>& notes, float amount)
{
    for (auto& n : notes) n.velocity = std::clamp (0.8f + (n.velocity - 0.8f) * amount, 0.02f, 1.0f);
}

bool readMidiNotes (const std::vector<uint8_t>& b, std::vector<std::vector<Note>>& tracks, double& bpm)
{
    tracks.clear();
    bpm = 120.0;
    if (b.size() < 14 || b[0] != 'M' || b[1] != 'T' || b[2] != 'h' || b[3] != 'd') return false;
    const int ntr = (b[10] << 8) | b[11], ppq = (b[12] << 8) | b[13];
    size_t p = 14;
    double usPerQ = 500000.0;
    for (int t = 0; t < ntr; ++t)
    {
        if (p + 8 > b.size() || b[p] != 'M' || b[p + 1] != 'T') return false;
        const size_t len = (size_t) b[p + 4] << 24 | (size_t) b[p + 5] << 16 | (size_t) b[p + 6] << 8 | b[p + 7];
        size_t q = p + 8;
        const size_t end = std::min (b.size(), q + len);
        int64_t tick = 0;
        uint8_t status = 0;
        std::vector<Note> notes;
        std::map<int, size_t> open;
        auto secs = [&] (int64_t tk) { return (double) tk / ppq * usPerQ / 1e6; };
        while (q < end)
        {
            uint32_t dv = 0;
            while (q < end) { const uint8_t c = b[q++]; dv = (dv << 7) | (c & 0x7f); if (! (c & 0x80)) break; }
            tick += dv;
            if (q >= end) break;
            if (b[q] & 0x80) status = b[q++];
            if (status == 0xff)
            {
                const uint8_t type = b[q++];
                uint32_t l = 0;
                while (q < end) { const uint8_t c = b[q++]; l = (l << 7) | (c & 0x7f); if (! (c & 0x80)) break; }
                if (type == 0x51 && l == 3) usPerQ = (double) (b[q] << 16 | b[q + 1] << 8 | b[q + 2]);
                q += l;
                continue;
            }
            const uint8_t hi = status & 0xf0;
            const int nData = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
            const uint8_t d1 = b[q], d2 = nData > 1 ? b[q + 1] : 0;
            q += (size_t) nData;
            const int key = ((status & 0x0f) << 8) | d1;
            if (hi == 0x90 && d2 > 0) { Note n; n.start = secs (tick); n.pitch = d1; n.velocity = d2 / 127.0f; open[key] = notes.size(); notes.push_back (n); }
            else if (hi == 0x80 || (hi == 0x90 && d2 == 0))
            {
                auto it = open.find (key);
                if (it != open.end()) { notes[it->second].end = secs (tick); open.erase (it); }
            }
        }
        tracks.push_back (notes);
        p = end;
    }
    bpm = 60e6 / usPerQ;
    return true;
}
} // namespace ss
