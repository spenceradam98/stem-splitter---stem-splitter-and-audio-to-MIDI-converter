// Source separation behind an interface, so a better model can be swapped in later.
// The default engine is HTDemucs (Demucs v4, 4 stems, MIT) run by demucs.cpp (MIT, Eigen only).
#pragma once

#include "Model.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ss
{
using ProgressFn = std::function<void (float progress, const std::string& stage)>;

class Separator
{
public:
    virtual ~Separator() = default;
    virtual std::string name() const = 0;
    virtual bool isReady() const = 0;
    virtual std::vector<StemKind> stemKinds() const = 0;
    virtual double sampleRate() const { return 44100.0; }

    // `in` is stereo at sampleRate(). On success `out` holds one stereo buffer per stem kind.
    virtual bool separate (const AudioBuffer& in, std::vector<AudioBuffer>& out, const ProgressFn& progress,
                           const std::atomic<bool>* cancel, std::string& error) = 0;
};

// Name of the bundled weights file, and where the installers put it.
const char* defaultModelFileName();                     // ggml-model-htdemucs-4s-f16.bin
std::vector<std::string> defaultModelSearchPaths();     // per-platform install folders + $STEMSCRIBE_MODEL
std::string findDefaultModel();                         // first existing file, or empty

// Creates the HTDemucs separator if this build includes demucs.cpp, else nullptr.
std::unique_ptr<Separator> createDemucsSeparator (const std::string& modelPath, int threads, std::string& error);
bool demucsAvailable();
} // namespace ss
