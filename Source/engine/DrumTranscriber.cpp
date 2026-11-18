#include "DrumTranscriber.h"

#include "Dsp.h"

#include <algorithm>
#include <cmath>

namespace ss
{
const char* gmDrumName (int note)
{
    switch (note)
    {
        case 35: case 36: return "Kick";
        case 37: return "Side Stick";
        case 38: case 40: return "Snare";
        case 39: return "Clap";
        case 41: case 43: case 45: case 47: case 48: case 50: return "Tom";
        case 42: return "Closed Hat";
        case 44: return "Pedal Hat";
        case 46: return "Open Hat";
        case 49: case 57: return "Crash";
        case 51: case 59: return "Ride";
        default: return "Perc";
    }
}

std::vector<Note> transcribeDrums (const std::vector<float>& x, double sr, const DrumSettings& set)
{
    std::vector<Note> out;
    if (x.size() < 2048) return out;
    const int n = 1024, hop = 256;
    const auto mag = stftMagnitude (x, n, hop);
    const size_t F = mag.size();
    const double binHz = sr / n, dt = hop / sr;
    enum { Low, LowMid, Mid, High, NB };
    const double edges[NB][2] = { { 30, 120 }, { 150, 400 }, { 1000, 5000 }, { 7000, 16000 } };
    std::vector<std::vector<float>> E (NB, std::vector<float> (F));
    for (size_t f = 0; f < F; ++f)
        for (int b = 0; b < NB; ++b)
        {
            double s = 0;
            const int k0 = std::max (1, (int) (edges[b][0] / binHz)), k1 = std::min ((int) mag[f].size() - 1, (int) (edges[b][1] / binHz));
            for (int k = k0; k <= k1; ++k) s += (double) mag[f][(size_t) k] * mag[f][(size_t) k];
            E[(size_t) b][f] = (float) (10.0 * std::log10 (s + 1e-9));
        }
    float ref[NB];
    for (int b = 0; b < NB; ++b) ref[b] = percentile (E[(size_t) b], 0.97f);

    // combined onset function: sum of positive band rises (dB), each band weighted equally
    std::vector<float> flux (F, 0.0f);
    for (size_t f = 2; f < F; ++f)
        for (int b = 0; b < NB; ++b)
        {
            if (E[(size_t) b][f] < ref[b] - 40) continue;
            const float r = E[(size_t) b][f] - std::max (E[(size_t) b][f - 1], E[(size_t) b][f - 2]);
            if (r > 0) flux[f] += r;
        }
    const auto med = medianFilter (flux, 16);
    const float floorFlux = percentile (flux, 0.9f);
    const float sens = std::max (0.1f, set.sensitivity);
    const int minGap = (int) std::ceil (0.03 / dt);
    long last = -minGap;
    for (size_t f = 3; f + 3 < F; ++f)
    {
        const float th = med[f] + 0.35f * floorFlux / sens + 3.0f / sens;
        if (! (flux[f] > th && flux[f] >= flux[f - 1] && flux[f] >= flux[f + 1]) || (long) f - last < minGap) continue;
        last = (long) f;
        float rise[NB], lvl[NB];
        for (int b = 0; b < NB; ++b)
        {
            float before = 1e9f, peak = -1e9f;
            for (size_t k = f - 3; k < f; ++k) before = std::min (before, E[(size_t) b][k]);
            for (size_t k = f; k <= f + 2; ++k) peak = std::max (peak, E[(size_t) b][k]);
            rise[b] = peak - before;
            lvl[b] = peak - ref[b];
        }
        const float need = 6.0f / sens, floorDb = -14.0f - 6.0f * (sens - 1.0f);
        const double t = (double) f * dt;
        auto add = [&] (int note, float level) {
            Note h;
            // frames are centred, so the rise shows up about half a window before the hit
            h.start = std::max (0.0, t + 0.5 * n / sr - 1.5 * dt);
            h.end = h.start + set.hitLength;
            h.pitch = note;
            h.velocity = std::clamp ((level + 24.0f) / 24.0f, 0.2f, 1.0f);
            out.push_back (h);
        };
        const bool kick = rise[Low] >= need && lvl[Low] >= floorDb;
        const bool snare = rise[Mid] >= need && lvl[Mid] >= floorDb && lvl[Mid] >= lvl[High] - 4.0f && (rise[LowMid] >= need * 0.5f || lvl[Mid] > lvl[High] + 2.0f);
        bool hat = rise[High] >= need * 0.8f && lvl[High] >= floorDb - 4.0f && (! snare || lvl[High] > lvl[Mid] + 6.0f);
        if (kick) add (gm::kick, lvl[Low]);
        if (snare) add (gm::snare, lvl[Mid]);
        if (hat)
        {
            // decay of the high band: open hats ring for longer than ~150 ms
            const float pk = E[High][f + 1];
            size_t k = f + 1;
            while (k < F && E[High][k] > pk - 9.0f && (double) (k - f) * dt < 0.6) ++k;
            add ((double) (k - f) * dt > 0.09 ? gm::openHat : gm::closedHat, lvl[High]);
        }
    }
    std::sort (out.begin(), out.end());
    return out;
}
} // namespace ss
