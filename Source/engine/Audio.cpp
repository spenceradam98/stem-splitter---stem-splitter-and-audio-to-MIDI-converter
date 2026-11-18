#include "Audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace ss
{
std::vector<float> AudioBuffer::mono() const
{
    std::vector<float> m (numFrames(), 0.0f);
    if (ch.empty()) return m;
    const float g = 1.0f / (float) ch.size();
    for (auto& c : ch)
        for (size_t i = 0; i < m.size(); ++i) m[i] += c[i] * g;
    return m;
}

AudioBuffer AudioBuffer::toStereo() const
{
    AudioBuffer s;
    s.sampleRate = sampleRate;
    if (ch.empty()) { s.setSize (2, 0); return s; }
    s.ch.push_back (ch[0]);
    s.ch.push_back (ch.size() > 1 ? ch[1] : ch[0]);
    return s;
}

float AudioBuffer::peak() const
{
    float p = 0.0f;
    for (auto& c : ch) for (float v : c) p = std::max (p, std::abs (v));
    return p;
}

uint64_t AudioBuffer::hash() const
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&] (uint64_t v) { for (int b = 0; b < 8; ++b) { h ^= (v >> (b * 8)) & 0xff; h *= 1099511628211ull; } };
    mix ((uint64_t) std::llround (sampleRate));
    mix ((uint64_t) ch.size());
    mix ((uint64_t) numFrames());
    for (auto& c : ch)
        for (float v : c)
            mix ((uint64_t) (uint16_t) (int16_t) std::lround (std::clamp (v, -1.0f, 1.0f) * 32767.0f));
    return h;
}

namespace
{
double besselI0 (double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; ++k)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}
} // namespace

std::vector<float> resample (const std::vector<float>& in, double fromRate, double toRate, int halfTaps)
{
    if (in.empty() || fromRate <= 0 || toRate <= 0) return {};
    if (std::abs (fromRate - toRate) < 1e-9) return in;
    const double ratio = toRate / fromRate;
    const size_t outLen = (size_t) std::floor ((double) in.size() * ratio);
    std::vector<float> out (outLen, 0.0f);
    const double cutoff = std::min (1.0, ratio) * 0.97;    // relative to the input Nyquist
    const double step = 1.0 / ratio;
    const double beta = 8.0, i0b = besselI0 (beta);
    // tabulated kernel: oversampled by 512 per input-sample unit
    const int over = 512;
    const double widthIn = halfTaps / cutoff;               // half-width in input samples
    const int tabLen = (int) std::ceil (widthIn * over) + 2;
    std::vector<float> tab ((size_t) tabLen);
    for (int k = 0; k < tabLen; ++k)
    {
        const double x = (double) k / over;                 // input-sample distance
        const double r = x / widthIn;
        if (r >= 1.0) { tab[(size_t) k] = 0.0f; continue; }
        const double t = M_PI * x * cutoff;
        const double sinc = x == 0.0 ? 1.0 : std::sin (t) / t;
        tab[(size_t) k] = (float) (cutoff * sinc * besselI0 (beta * std::sqrt (1.0 - r * r)) / i0b);
    }
    const long n = (long) in.size();
    for (size_t o = 0; o < outLen; ++o)
    {
        const double pos = (double) o * step;
        const long c = (long) std::floor (pos);
        const long lo = std::max (0L, c - (long) std::ceil (widthIn) + 1), hi = std::min (n - 1, c + (long) std::ceil (widthIn));
        double acc = 0.0;
        for (long j = lo; j <= hi; ++j)
        {
            const double d = std::abs (pos - (double) j) * over;
            const int k = (int) d;
            if (k + 1 >= tabLen) continue;
            const double fr = d - k;
            acc += in[(size_t) j] * (tab[(size_t) k] + fr * (tab[(size_t) k + 1] - tab[(size_t) k]));
        }
        out[o] = (float) acc;
    }
    return out;
}

AudioBuffer resample (const AudioBuffer& in, double toRate)
{
    AudioBuffer out;
    out.sampleRate = toRate;
    for (auto& c : in.ch) out.ch.push_back (resample (c, in.sampleRate, toRate));
    return out;
}

// ------------------------------------------------------------------------------------------
namespace
{
uint32_t rd32 (const uint8_t* p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
uint16_t rd16 (const uint8_t* p) { return (uint16_t) (p[0] | (p[1] << 8)); }
void wr32 (std::vector<uint8_t>& v, uint32_t x) { for (int b = 0; b < 4; ++b) v.push_back ((uint8_t) (x >> (8 * b))); }
void wr16 (std::vector<uint8_t>& v, uint16_t x) { v.push_back ((uint8_t) x); v.push_back ((uint8_t) (x >> 8)); }
} // namespace

bool readWav (const std::string& path, AudioBuffer& out, std::string* error)
{
    auto fail = [&] (const char* m) { if (error) *error = m; return false; };
    std::ifstream f (path, std::ios::binary);
    if (! f) return fail ("cannot open file");
    std::vector<uint8_t> d ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp (d.data(), "RIFF", 4) != 0 || std::memcmp (d.data() + 8, "WAVE", 4) != 0)
        return fail ("not a RIFF/WAVE file");
    int fmt = 0, chans = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t* data = nullptr;
    size_t dataLen = 0;
    size_t p = 12;
    while (p + 8 <= d.size())
    {
        const uint32_t len = rd32 (&d[p + 4]);
        const uint8_t* body = &d[p + 8];
        const size_t avail = std::min<size_t> (len, d.size() - (p + 8));
        if (std::memcmp (&d[p], "fmt ", 4) == 0 && avail >= 16)
        {
            fmt = rd16 (body); chans = rd16 (body + 2); rate = rd32 (body + 4); bits = rd16 (body + 14);
            if (fmt == 0xFFFE && avail >= 26) fmt = rd16 (body + 24);   // WAVE_FORMAT_EXTENSIBLE sub-format
        }
        else if (std::memcmp (&d[p], "data", 4) == 0) { data = body; dataLen = avail; }
        p += 8 + len + (len & 1);
    }
    if (! data || chans <= 0 || rate == 0) return fail ("missing fmt or data chunk");
    if (! ((fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && (bits == 32 || bits == 64))))
        return fail ("unsupported WAV sample format");
    const size_t bps = (size_t) bits / 8, frames = dataLen / (bps * (size_t) chans);
    out.sampleRate = rate;
    out.setSize (chans, frames);
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < chans; ++c)
        {
            const uint8_t* s = data + (i * (size_t) chans + (size_t) c) * bps;
            float v = 0.0f;
            if (fmt == 3 && bits == 32) { float x; std::memcpy (&x, s, 4); v = x; }
            else if (fmt == 3) { double x; std::memcpy (&x, s, 8); v = (float) x; }
            else if (bits == 8) v = ((int) s[0] - 128) / 128.0f;
            else if (bits == 16) v = (int16_t) rd16 (s) / 32768.0f;
            else if (bits == 24) { int32_t x = (int32_t) ((uint32_t) s[0] << 8 | (uint32_t) s[1] << 16 | (uint32_t) s[2] << 24) >> 8; v = (float) x / 8388608.0f; }
            else v = (float) ((double) (int32_t) rd32 (s) / 2147483648.0);
            out.ch[(size_t) c][i] = v;
        }
    return true;
}

std::vector<uint8_t> encodeWav (const AudioBuffer& in, int bits)
{
    if (bits != 16 && bits != 24 && bits != 32) bits = 24;
    const bool isFloat = bits == 32;
    const int chans = std::max (1, in.numChannels());
    const size_t frames = in.numFrames();
    const uint32_t dataLen = (uint32_t) (frames * (size_t) chans * (size_t) (bits / 8));
    std::vector<uint8_t> v;
    v.reserve (44 + dataLen);
    v.insert (v.end(), { 'R', 'I', 'F', 'F' }); wr32 (v, 36 + dataLen);
    v.insert (v.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' }); wr32 (v, 16);
    wr16 (v, isFloat ? 3 : 1); wr16 (v, (uint16_t) chans);
    const uint32_t rate = (uint32_t) std::lround (in.sampleRate);
    wr32 (v, rate); wr32 (v, rate * (uint32_t) chans * (uint32_t) (bits / 8));
    wr16 (v, (uint16_t) (chans * bits / 8)); wr16 (v, (uint16_t) bits);
    v.insert (v.end(), { 'd', 'a', 't', 'a' }); wr32 (v, dataLen);
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < chans; ++c)
        {
            const float x = in.ch.empty() ? 0.0f : std::clamp (in.ch[(size_t) std::min (c, in.numChannels() - 1)][i], -1.0f, 1.0f);
            if (isFloat) { uint32_t u; std::memcpy (&u, &x, 4); wr32 (v, u); }
            else if (bits == 16) wr16 (v, (uint16_t) (int16_t) std::lround (x * 32767.0f));
            else { const int32_t s = (int32_t) std::lround (x * 8388607.0f); v.push_back ((uint8_t) s); v.push_back ((uint8_t) (s >> 8)); v.push_back ((uint8_t) (s >> 16)); }
        }
    return v;
}

bool writeWav (const std::string& path, const AudioBuffer& in, int bits, std::string* error)
{
    auto bytes = encodeWav (in, bits);
    std::ofstream f (path, std::ios::binary);
    if (! f) { if (error) *error = "cannot write file"; return false; }
    f.write ((const char*) bytes.data(), (std::streamsize) bytes.size());
    return (bool) f;
}
} // namespace ss
