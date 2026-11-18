#include "Separator.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

#if STEMSCRIBE_WITH_DEMUCS
 #include "model.hpp"
#endif

namespace ss
{
const char* defaultModelFileName() { return "ggml-model-htdemucs-4s-f16.bin"; }

std::vector<std::string> defaultModelSearchPaths()
{
    std::vector<std::string> p;
    if (const char* env = std::getenv ("STEMSCRIBE_MODEL")) p.push_back (env);
    const std::string f = defaultModelFileName();
#if defined(_WIN32)
    if (const char* pd = std::getenv ("PROGRAMDATA")) p.push_back (std::string (pd) + "\\YourBrand\\stem-scribe\\" + f);
    if (const char* la = std::getenv ("LOCALAPPDATA")) p.push_back (std::string (la) + "\\YourBrand\\stem-scribe\\" + f);
#elif defined(__APPLE__)
    p.push_back ("/Library/Application Support/YourBrand/stem-scribe/" + f);
    if (const char* h = std::getenv ("HOME")) p.push_back (std::string (h) + "/Library/Application Support/YourBrand/stem-scribe/" + f);
#else
    if (const char* h = std::getenv ("HOME")) p.push_back (std::string (h) + "/.local/share/stem-scribe/" + f);
    p.push_back ("/usr/local/share/stem-scribe/" + f);
#endif
    p.push_back ("models/" + f);
    return p;
}

std::string findDefaultModel()
{
    std::error_code ec;
    for (auto& p : defaultModelSearchPaths())
        if (std::filesystem::is_regular_file (p, ec)) return p;
    return {};
}

#if STEMSCRIBE_WITH_DEMUCS
namespace
{
struct Cancelled {};

// HTDemucs through demucs.cpp. The song is cut into one overlapping section per thread (the same
// idea as demucs.cpp's threaded driver); each section runs the normal 7.8 s segment inference and the
// sections are cross-faded back together.
class DemucsSeparator : public Separator
{
public:
    bool load (const std::string& path, int threadCount, std::string& err)
    {
        threads = threadCount;
        model = std::make_unique<demucscpp::demucs_model>();
        if (! demucscpp::load_demucs_model (path, model.get())) { err = "could not load the separation model: " + path; model.reset(); return false; }
        return true;
    }

    std::string name() const override { return model && ! model->is_4sources ? "HTDemucs 6-stem" : "HTDemucs 4-stem"; }
    bool isReady() const override { return model != nullptr; }
    std::vector<StemKind> stemKinds() const override
    {
        // demucs target order: drums, bass, other, vocals (, guitar, piano)
        std::vector<StemKind> k { StemKind::Drums, StemKind::Bass, StemKind::Other, StemKind::Vocals };
        if (model && ! model->is_4sources) { k.push_back (StemKind::Guitar); k.push_back (StemKind::Piano); }
        return k;
    }

    bool separate (const AudioBuffer& inRaw, std::vector<AudioBuffer>& out, const ProgressFn& progress,
                   const std::atomic<bool>* cancel, std::string& err) override
    {
        if (! model) { err = "no model loaded"; return false; }
        const AudioBuffer in = inRaw.toStereo();
        const long total = (long) in.numFrames();
        const int nSrc = model->is_4sources ? 4 : 6;
        // one section per thread, but never shorter than ~20 s (short sections waste work on overlap)
        int n = threads > 0 ? threads : (int) std::max (1u, std::thread::hardware_concurrency());
        n = std::clamp (n, 1, std::max (1, (int) (total / (44100L * 20))));
        const long ov = 44100L * 3 / 4;   // 0.75 s overlap
        const long segLen = (total + n - 1) / n;

        std::vector<Eigen::Tensor3dXf> results ((size_t) n);
        std::vector<float> prog ((size_t) n, 0.0f);
        std::vector<std::string> errs ((size_t) n);
        std::mutex lock;
        std::vector<std::thread> pool;
        for (int i = 0; i < n; ++i)
        {
            pool.emplace_back ([&, i] {
                const long a = std::max (0L, i * segLen - ov), b = std::min (total, (i + 1) * segLen + ov);
                Eigen::MatrixXf seg (2, std::max (1L, b - a));
                for (long k = a; k < b; ++k) { seg (0, k - a) = in.ch[0][(size_t) k]; seg (1, k - a) = in.ch[1][(size_t) k]; }
                demucscpp::ProgressCallback cb = [&, i] (float p, const std::string&) {
                    if (cancel && cancel->load()) throw Cancelled {};
                    std::lock_guard<std::mutex> l (lock);
                    prog[(size_t) i] = p;
                    float s = 0; for (float v : prog) s += v;
                    if (progress) progress (s / (float) prog.size(), "Separating stems");
                };
                try { results[(size_t) i] = demucscpp::demucs_inference (*model, seg, cb); }
                catch (const Cancelled&) { errs[(size_t) i] = "cancelled"; }
                catch (const std::exception& e) { errs[(size_t) i] = e.what(); }
            });
        }
        for (auto& t : pool) t.join();
        for (auto& e : errs) if (! e.empty()) { err = e; return false; }

        out.assign ((size_t) nSrc, AudioBuffer());
        for (auto& o : out) { o.sampleRate = 44100.0; o.setSize (2, (size_t) total); }
        std::vector<float> wsum ((size_t) total, 0.0f);
        for (int i = 0; i < n; ++i)
        {
            const long a = std::max (0L, i * segLen - ov), b = std::min (total, (i + 1) * segLen + ov);
            const long core0 = i * segLen, core1 = std::min (total, (i + 1) * segLen);
            for (long k = a; k < b; ++k)
            {
                // weight 1 inside the section's own range, linear ramps across the overlaps
                float w = 1.0f;
                if (k < core0) w = (float) (k - a + 1) / (float) (core0 - a + 1);
                else if (k >= core1) w = (float) (b - k) / (float) (b - core1 + 1);
                wsum[(size_t) k] += w;
                for (int s = 0; s < nSrc; ++s)
                    for (int c = 0; c < 2; ++c)
                        out[(size_t) s].ch[(size_t) c][(size_t) k] += w * results[(size_t) i] (s, c, k - a);
            }
        }
        for (auto& o : out)
            for (auto& c : o.ch)
                for (long k = 0; k < total; ++k) if (wsum[(size_t) k] > 0) c[(size_t) k] /= wsum[(size_t) k];
        return true;
    }

private:
    std::unique_ptr<demucscpp::demucs_model> model;
    int threads = 0;
};
} // namespace

bool demucsAvailable() { return true; }

std::unique_ptr<Separator> createDemucsSeparator (const std::string& path, int threads, std::string& err)
{
    auto s = std::make_unique<DemucsSeparator>();
    if (! s->load (path, threads, err)) return nullptr;
    return s;
}
#else
bool demucsAvailable() { return false; }
std::unique_ptr<Separator> createDemucsSeparator (const std::string&, int, std::string& err)
{
    err = "this build does not include the separation engine";
    return nullptr;
}
#endif
} // namespace ss
