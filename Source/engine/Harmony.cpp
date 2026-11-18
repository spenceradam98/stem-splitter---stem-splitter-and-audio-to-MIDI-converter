#include "Harmony.h"

#include "Dsp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace ss
{
TempoEstimate estimateTempo (const std::vector<float>& mono, double sr)
{
    TempoEstimate te;
    std::vector<float> env;
    detectOnsets (mono, sr, &env);
    const int hop = std::max (1, (int) std::lround (sr / 200.0));
    const double dt = (double) hop / sr;
    if (env.size() < 400) return te;
    // remove the slow trend
    const auto base = medianFilter (env, 25);
    for (size_t i = 0; i < env.size(); ++i) env[i] = std::max (0.0f, env[i] - base[i]);

    double best = -1; int bestLag = 0;
    const int lagLo = (int) std::floor (60.0 / 200.0 / dt), lagHi = (int) std::ceil (60.0 / 60.0 / dt);
    std::vector<double> ac ((size_t) lagHi + 2, 0.0);
    for (int lag = lagLo; lag <= lagHi + 1; ++lag)
    {
        double s = 0;
        for (size_t i = (size_t) lag; i < env.size(); ++i) s += (double) env[i] * env[i - (size_t) lag];
        ac[(size_t) lag] = s / (double) (env.size() - (size_t) lag);
    }
    double total = 0;
    for (int lag = lagLo; lag <= lagHi; ++lag)
    {
        const double bpm = 60.0 / (lag * dt);
        const double prior = std::exp (-0.5 * std::pow (std::log2 (bpm / 120.0) / 0.9, 2.0));
        // reward lags whose double also correlates (metrical structure)
        const double s = (ac[(size_t) lag] + 0.5 * (2 * lag <= lagHi + 1 ? ac[(size_t) (2 * lag)] : 0.0)) * prior;
        total += s;
        if (s > best) { best = s; bestLag = lag; }
    }
    if (bestLag <= 0) return te;
    // parabolic refinement of the lag
    double lagF = bestLag;
    if (bestLag > lagLo && bestLag < lagHi)
    {
        const double a = ac[(size_t) bestLag - 1], b = ac[(size_t) bestLag], c = ac[(size_t) bestLag + 1];
        const double den = a - 2 * b + c;
        if (std::abs (den) > 1e-12) lagF += std::clamp (0.5 * (a - c) / den, -0.5, 0.5);
    }
    te.bpm = 60.0 / (lagF * dt);
    te.confidence = total > 0 ? (float) (best / total * (lagHi - lagLo)) / 10.0f : 0.0f;
    // phase of the grid
    double bestPhase = -1;
    const double period = lagF;
    for (int ph = 0; ph < (int) period; ++ph)
    {
        double s = 0;
        for (double p = ph; p < (double) env.size(); p += period)
        {
            const long i = (long) std::lround (p);
            float m = 0;
            for (long j = i - 1; j <= i + 1; ++j) if (j >= 0 && j < (long) env.size()) m = std::max (m, env[(size_t) j]);
            s += m;
        }
        if (s > bestPhase) { bestPhase = s; te.offset = ph * dt; }
    }
    return te;
}

void detectKey (const std::vector<const std::vector<Note>*>& sets, int& root, bool& minor)
{
    static const double maj[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static const double mnr[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
    std::array<double, 12> pc {};
    for (auto* s : sets)
        if (s) for (auto& n : *s) pc[(size_t) (((n.pitch % 12) + 12) % 12)] += std::max (0.0, n.end - n.start) * (0.5 + n.velocity);
    auto corr = [] (const std::array<double, 12>& a, const double* prof, int shift) {
        double ma = 0, mp = 0;
        for (int i = 0; i < 12; ++i) { ma += a[(size_t) i]; mp += prof[i]; }
        ma /= 12; mp /= 12;
        double num = 0, da = 0, dp = 0;
        for (int i = 0; i < 12; ++i)
        {
            const double x = a[(size_t) ((i + shift) % 12)] - ma, y = prof[i] - mp;
            num += x * y; da += x * x; dp += y * y;
        }
        return da > 0 && dp > 0 ? num / std::sqrt (da * dp) : 0.0;
    };
    double best = -2;
    root = 0; minor = false;
    for (int r = 0; r < 12; ++r)
    {
        const double a = corr (pc, maj, r), b = corr (pc, mnr, r);
        if (a > best) { best = a; root = r; minor = false; }
        if (b > best) { best = b; root = r; minor = true; }
    }
    // relative major / minor are often nearly tied; major keys are far more common, so a minor
    // key has to win clearly
    if (minor && corr (pc, maj, (root + 3) % 12) > best - 0.05) { root = (root + 3) % 12; minor = false; }
}

std::vector<Chord> detectChords (const std::vector<const std::vector<Note>*>& harmonic, const std::vector<Note>* bass,
                                 double duration, double bpm, double offset, const ChordSettings& set)
{
    std::vector<Chord> out;
    if (duration <= 0) return out;
    const double step = (bpm > 20 ? 60.0 / bpm : 0.5) * std::max (0.25, set.beatsPerFrame);
    double t0 = std::fmod (offset, step);
    if (t0 > 1e-6) t0 -= step;
    std::vector<double> bounds;
    for (double t = t0; t < duration; t += step) bounds.push_back (std::max (0.0, t));
    bounds.push_back (duration);
    const size_t F = bounds.size() - 1;
    if (F == 0) return out;

    std::vector<std::array<float, 12>> chroma (F), bchroma (F);
    for (auto& c : chroma) c.fill (0); for (auto& c : bchroma) c.fill (0);
    auto accumulate = [&] (const std::vector<Note>& notes, float w, bool isBass) {
        for (auto& n : notes)
        {
            auto it = std::upper_bound (bounds.begin(), bounds.end(), n.start);
            size_t f = it == bounds.begin() ? 0 : (size_t) (it - bounds.begin() - 1);
            for (; f < F && bounds[f] < n.end; ++f)
            {
                const double ov = std::min (n.end, bounds[f + 1]) - std::max (n.start, bounds[f]);
                if (ov <= 0) continue;
                const int pc = ((n.pitch % 12) + 12) % 12;
                const float v = (float) (ov / step) * w * (0.4f + n.velocity);
                chroma[f][(size_t) pc] += v;
                if (isBass) bchroma[f][(size_t) pc] += v;
            }
        }
    };
    for (auto* h : harmonic) if (h) accumulate (*h, 1.0f, false);
    if (bass) accumulate (*bass, 1.2f, true);

    const auto& qs = chordQualities();
    const int Q = (int) qs.size(), S = 12 * Q + 1;   // last state = N
    std::vector<std::array<float, 12>> templ ((size_t) S - 1);
    for (int r = 0; r < 12; ++r)
        for (int q = 0; q < Q; ++q)
        {
            auto& t = templ[(size_t) (r * Q + q)];
            t.fill (0);
            for (size_t k = 0; k < qs[(size_t) q].intervals.size(); ++k) t[(size_t) ((r + qs[(size_t) q].intervals[k]) % 12)] = k == 0 ? 1.2f : 1.0f;
        }
    // simpler chords are preferred when the evidence is equal
    auto qPrior = [&] (int q) { const auto n = qs[(size_t) q].intervals.size(); return q <= 1 ? 1.0f : (n == 3 ? 0.95f : 0.93f); };
    float energyRef = 1e-6f;
    std::vector<float> energy (F);
    for (size_t f = 0; f < F; ++f) { energy[f] = std::accumulate (chroma[f].begin(), chroma[f].end(), 0.0f); energyRef = std::max (energyRef, energy[f]); }

    std::vector<std::vector<float>> obs (F, std::vector<float> ((size_t) S, 0.0f));
    for (size_t f = 0; f < F; ++f)
    {
        const auto& c = chroma[f];
        float cn = 0;
        for (float v : c) cn += v * v;
        cn = std::sqrt (cn);
        float bassMax = 0; int bassPc = -1;
        for (int k = 0; k < 12; ++k) if (bchroma[f][(size_t) k] > bassMax) { bassMax = bchroma[f][(size_t) k]; bassPc = k; }
        for (int s = 0; s < S - 1; ++s)
        {
            const auto& t = templ[(size_t) s];
            float dot = 0, tn = 0;
            for (int k = 0; k < 12; ++k) { dot += c[(size_t) k] * t[(size_t) k]; tn += t[(size_t) k] * t[(size_t) k]; }
            float sim = cn > 0 ? dot / (cn * std::sqrt (tn)) : 0.0f;
            sim *= qPrior (s % Q);
            if (bassPc >= 0 && bassPc == s / Q) sim += 0.08f;
            obs[f][(size_t) s] = sim;
        }
        obs[f][(size_t) S - 1] = energy[f] < 0.05f * energyRef ? 1.0f : 0.25f;
    }
    // Viterbi with a constant change penalty
    std::vector<float> score = obs[0];
    std::vector<std::vector<int>> back (F, std::vector<int> ((size_t) S, 0));
    for (size_t f = 1; f < F; ++f)
    {
        const int bestPrev = (int) (std::max_element (score.begin(), score.end()) - score.begin());
        std::vector<float> nx ((size_t) S);
        for (int s = 0; s < S; ++s)
        {
            const float stay = score[(size_t) s], jump = score[(size_t) bestPrev] - set.changePenalty;
            if (stay >= jump) { nx[(size_t) s] = stay + obs[f][(size_t) s]; back[f][(size_t) s] = s; }
            else { nx[(size_t) s] = jump + obs[f][(size_t) s]; back[f][(size_t) s] = bestPrev; }
        }
        score = nx;
    }
    std::vector<int> path (F);
    path[F - 1] = (int) (std::max_element (score.begin(), score.end()) - score.begin());
    for (size_t f = F - 1; f > 0; --f) path[f - 1] = back[f][(size_t) path[f]];

    for (size_t f = 0; f < F;)
    {
        size_t g = f;
        while (g < F && path[g] == path[f]) ++g;
        Chord c;
        c.start = bounds[f]; c.end = bounds[g];
        if (path[f] == S - 1) c.root = -1;
        else
        {
            c.root = path[f] / Q; c.quality = path[f] % Q;
            if (set.slashChords)
            {
                std::array<float, 12> b {};
                for (size_t k = f; k < g; ++k) for (int p = 0; p < 12; ++p) b[(size_t) p] += bchroma[k][(size_t) p];
                const int bp = (int) (std::max_element (b.begin(), b.end()) - b.begin());
                const float tot = std::accumulate (b.begin(), b.end(), 0.0f);
                if (tot > 0 && bp != c.root && b[(size_t) bp] > 0.6f * tot)
                {
                    bool inChord = false;
                    for (int iv : qs[(size_t) c.quality].intervals) inChord = inChord || (c.root + iv) % 12 == bp;
                    if (inChord) c.bass = bp;
                }
            }
        }
        out.push_back (c);
        f = g;
    }
    return out;
}
} // namespace ss
