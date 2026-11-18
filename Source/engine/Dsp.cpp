#include "Dsp.h"

#include <algorithm>
#include <cmath>

namespace ss
{
int nextPow2 (int n) { int p = 1; while (p < n) p <<= 1; return p; }

void fft (std::vector<std::complex<float>>& a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = 2.0 * M_PI / (double) len * (inverse ? 1 : -1);
        const std::complex<float> wl ((float) std::cos (ang), (float) std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<float> w (1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse) for (auto& v : a) v /= (float) n;
}

std::vector<std::vector<float>> stftMagnitude (const std::vector<float>& x, int n, int hop)
{
    std::vector<float> win ((size_t) n);
    for (int i = 0; i < n; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * (float) M_PI * i / n);
    const size_t frames = x.size() / (size_t) hop + 1;
    std::vector<std::vector<float>> out (frames, std::vector<float> ((size_t) n / 2 + 1));
    std::vector<std::complex<float>> buf ((size_t) n);
    for (size_t f = 0; f < frames; ++f)
    {
        const long start = (long) f * hop - n / 2;
        for (int i = 0; i < n; ++i)
        {
            const long s = start + i;
            buf[(size_t) i] = { s >= 0 && s < (long) x.size() ? x[(size_t) s] * win[(size_t) i] : 0.0f, 0.0f };
        }
        fft (buf);
        for (int k = 0; k <= n / 2; ++k) out[f][(size_t) k] = std::abs (buf[(size_t) k]);
    }
    return out;
}

float median (std::vector<float> v)
{
    if (v.empty()) return 0.0f;
    auto mid = v.begin() + (long) v.size() / 2;
    std::nth_element (v.begin(), mid, v.end());
    return *mid;
}

float percentile (std::vector<float> v, float p)
{
    if (v.empty()) return 0.0f;
    const size_t k = std::min (v.size() - 1, (size_t) std::lround (std::clamp (p, 0.0f, 1.0f) * (float) (v.size() - 1)));
    std::nth_element (v.begin(), v.begin() + (long) k, v.end());
    return v[k];
}

std::vector<float> medianFilter (const std::vector<float>& x, int radius)
{
    std::vector<float> y (x.size());
    std::vector<float> w;
    for (size_t i = 0; i < x.size(); ++i)
    {
        w.clear();
        for (long j = (long) i - radius; j <= (long) i + radius; ++j)
            if (j >= 0 && j < (long) x.size()) w.push_back (x[(size_t) j]);
        y[i] = median (w);
    }
    return y;
}

std::vector<float> rmsEnvelope (const std::vector<float>& x, int hop)
{
    const size_t frames = x.size() / (size_t) hop + 1;
    std::vector<float> r (frames);
    for (size_t f = 0; f < frames; ++f)
    {
        double s = 0; int n = 0;
        for (long i = (long) f * hop - hop; i < (long) f * hop + hop; ++i)
            if (i >= 0 && i < (long) x.size()) { s += (double) x[(size_t) i] * x[(size_t) i]; ++n; }
        r[f] = n ? (float) std::sqrt (s / n) : 0.0f;
    }
    return r;
}

std::vector<double> detectOnsets (const std::vector<float>& x, double sr, std::vector<float>* strength,
                                  float sensitivity, double minGapSeconds)
{
    const int n = 2048, hop = std::max (1, (int) std::lround (sr / 200.0));   // 5 ms hop
    auto mag = stftMagnitude (x, n, hop);
    std::vector<float> flux (mag.size(), 0.0f);
    for (size_t f = 1; f < mag.size(); ++f)
    {
        float s = 0;
        for (size_t k = 1; k < mag[f].size(); ++k)
        {
            const float d = std::log1p (100.0f * mag[f][k]) - std::log1p (100.0f * mag[f - 1][k]);
            if (d > 0) s += d;
        }
        flux[f] = s;
    }
    // adaptive threshold: local median plus a fraction of the global level
    const float global = percentile (flux, 0.995f);
    auto med = medianFilter (flux, 20);
    std::vector<double> onsets;
    const int minGap = std::max (1, (int) std::lround (minGapSeconds * sr / hop));
    long last = -minGap - 1;
    for (size_t f = 1; f + 1 < flux.size(); ++f)
    {
        const float th = med[f] + (0.25f / std::max (0.05f, sensitivity)) * global + 1e-3f;
        if (flux[f] > th && flux[f] >= flux[f - 1] && flux[f] >= flux[f + 1] && (long) f - last >= minGap)
        {
            onsets.push_back ((double) f * hop / sr);
            last = (long) f;
        }
    }
    if (strength) *strength = flux;
    return onsets;
}
} // namespace ss
