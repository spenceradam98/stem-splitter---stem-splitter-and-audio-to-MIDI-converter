// Small DSP helpers shared by the analysers: FFT, STFT magnitudes, filters. Scalar C++.
#pragma once

#include <cmath>
#include <complex>
#include <vector>

namespace ss
{
// In-place radix-2 complex FFT (size must be a power of two).
void fft (std::vector<std::complex<float>>& x, bool inverse = false);
int nextPow2 (int n);

// Magnitude spectrogram with a Hann window. Frame k covers samples [k*hop - n/2, k*hop + n/2).
// Returns frames x (n/2+1) magnitudes.
std::vector<std::vector<float>> stftMagnitude (const std::vector<float>& x, int n, int hop);

std::vector<float> medianFilter (const std::vector<float>& x, int radius);
float median (std::vector<float> v);
float percentile (std::vector<float> v, float p);   // p in 0..1

// RMS per hop (window = 2*hop, centred) in linear units.
std::vector<float> rmsEnvelope (const std::vector<float>& x, int hop);

// Spectral-flux onset detection (log-compressed magnitude rise, adaptive threshold).
// Returns onset times in seconds and fills `strength` (same length) if given.
std::vector<double> detectOnsets (const std::vector<float>& x, double sr, std::vector<float>* strength = nullptr,
                                  float sensitivity = 1.0f, double minGapSeconds = 0.05);

inline float hzToMidi (float hz) { return hz > 0 ? 69.0f + 12.0f * std::log2 (hz / 440.0f) : 0.0f; }
inline float midiToHz (float m) { return 440.0f * std::pow (2.0f, (m - 69.0f) / 12.0f); }
} // namespace ss
