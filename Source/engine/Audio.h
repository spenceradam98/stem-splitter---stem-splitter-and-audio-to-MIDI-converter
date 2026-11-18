// Audio buffers, WAV file I/O and sample-rate conversion. Plain C++17, no JUCE.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ss
{
// Planar stereo (or mono when R is empty) audio at a given sample rate.
struct AudioBuffer
{
    double sampleRate = 44100.0;
    std::vector<std::vector<float>> ch;   // ch[0] = left, ch[1] = right (optional)

    int numChannels() const { return (int) ch.size(); }
    size_t numFrames() const { return ch.empty() ? 0 : ch[0].size(); }
    double seconds() const { return sampleRate > 0 ? (double) numFrames() / sampleRate : 0.0; }
    void setSize (int channels, size_t frames) { ch.assign ((size_t) channels, std::vector<float> (frames, 0.0f)); }

    std::vector<float> mono() const;          // average of all channels
    AudioBuffer toStereo() const;             // duplicates a mono channel, drops extra channels
    float peak() const;
    uint64_t hash() const;                    // FNV-1a over 16-bit-quantised samples + rate (cache key)
};

// Windowed-sinc resampler (Kaiser window, anti-aliased when downsampling). Good enough for analysis
// and for stems that are played back by the plugin at the host rate.
std::vector<float> resample (const std::vector<float>& in, double fromRate, double toRate, int halfTaps = 24);
AudioBuffer resample (const AudioBuffer& in, double toRate);

// WAV I/O: reads 8/16/24/32-bit PCM and 32/64-bit float, mono or multichannel. Writes 16-bit PCM,
// 24-bit PCM or 32-bit float.
bool readWav (const std::string& path, AudioBuffer& out, std::string* error = nullptr);
bool writeWav (const std::string& path, const AudioBuffer& in, int bits = 24, std::string* error = nullptr);
std::vector<uint8_t> encodeWav (const AudioBuffer& in, int bits = 24);
} // namespace ss
