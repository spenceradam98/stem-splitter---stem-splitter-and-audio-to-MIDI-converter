// The full analysis: (optional) separation, per-stem transcription, tempo, key and chords,
// plus the on-disk cache keyed by a hash of the input audio.
#pragma once

#include "BasicPitch.h"
#include "Model.h"
#include "Separator.h"

#include <atomic>
#include <string>

namespace ss
{
struct PipelineOptions
{
    bool split = true;                      // false = plain audio-to-MIDI on the whole input
    Transcribe mixMode = Transcribe::Poly;  // how to transcribe the input when not splitting
    bool transcribe = true;
    bool chords = true;
    double hostBpm = 0.0;                   // > 0 when the host's tempo is known
    double hostBeatOffset = 0.0;            // seconds from the start of the audio to a beat
    int threads = 0;                        // 0 = all cores
};

// Runs everything. `separator` may be null (or not ready) when split is false.
bool runPipeline (const AudioBuffer& input, const PipelineOptions& o, Separator* separator, const BasicPitch& bp,
                  Analysis& out, const ProgressFn& progress, const std::atomic<bool>* cancel, std::string& error);

// (Re)transcribes one stem with its current mode, e.g. after the user switches Mono <-> Poly.
void transcribeStem (StemResult& s, const BasicPitch& bp, int threads, const std::atomic<bool>* cancel = nullptr,
                     const std::function<void (float)>& progress = {});

// Recomputes key and chords from the stems' current notes (after edits).
void updateHarmony (Analysis& a);

class AnalysisCache
{
public:
    explicit AnalysisCache (std::string rootDir);
    static std::string defaultRoot();         // per-user cache folder
    const std::string& root() const { return dir; }

    std::string folderFor (uint64_t key) const;
    bool has (uint64_t key) const;
    bool save (const Analysis& a) const;      // stems as float WAVs + analysis.bin
    bool saveData (const Analysis& a) const;  // analysis.bin only (after edits)
    bool load (uint64_t key, Analysis& out) const;
    std::string stemPath (uint64_t key, const StemResult& s) const;

private:
    std::string dir;
};

std::string hexKey (uint64_t key);
// Cache key of an analysis: hash of the input audio, different for split and unsplit runs.
uint64_t analysisKey (const AudioBuffer& input, bool split);
} // namespace ss
