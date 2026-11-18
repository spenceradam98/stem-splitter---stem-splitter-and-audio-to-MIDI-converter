#include "GraphRunner.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

namespace ss
{
size_t Tensor::size() const
{
    size_t n = 1;
    for (auto d : shape) n *= (size_t) d;
    return n;
}

Tensor Tensor::floats (std::vector<int64_t> s)
{
    Tensor t;
    t.shape = std::move (s);
    t.f.assign (t.size(), 0.0f);
    return t;
}

namespace
{
struct Reader
{
    const uint8_t* p; const uint8_t* end;
    bool ok = true;
    template <typename T> T get()
    {
        T v {};
        if (end - p < (ptrdiff_t) sizeof (T)) { ok = false; return v; }
        std::memcpy (&v, p, sizeof (T)); p += sizeof (T);
        return v;
    }
    std::string str()
    {
        auto n = get<uint16_t>();
        if (end - p < n) { ok = false; return {}; }
        std::string s ((const char*) p, n); p += n;
        return s;
    }
};

std::vector<int64_t> toInts (const Tensor& t)
{
    if (t.isInt) return t.i;
    std::vector<int64_t> r (t.f.size());
    for (size_t k = 0; k < r.size(); ++k) r[k] = (int64_t) t.f[k];
    return r;
}

std::vector<int64_t> stridesOf (const std::vector<int64_t>& shape)
{
    std::vector<int64_t> s (shape.size(), 1);
    for (int k = (int) shape.size() - 2; k >= 0; --k) s[(size_t) k] = s[(size_t) k + 1] * shape[(size_t) k + 1];
    return s;
}

int normAxis (int64_t a, size_t rank) { return (int) (a < 0 ? a + (int64_t) rank : a); }

std::vector<int64_t> broadcastShape (const std::vector<const Tensor*>& ts)
{
    size_t rank = 0;
    for (auto* t : ts) rank = std::max (rank, t->shape.size());
    std::vector<int64_t> out (rank, 1);
    for (auto* t : ts)
        for (size_t k = 0; k < t->shape.size(); ++k)
        {
            auto& o = out[rank - t->shape.size() + k];
            auto d = t->shape[k];
            if (d != 1) { if (o != 1 && o != d) throw std::runtime_error ("broadcast mismatch"); o = d; }
        }
    return out;
}

// strides of t when broadcast to `shape` (0 on broadcast dimensions)
std::vector<int64_t> bstrides (const Tensor& t, const std::vector<int64_t>& shape)
{
    auto s = stridesOf (t.shape);
    std::vector<int64_t> r (shape.size(), 0);
    for (size_t k = 0; k < t.shape.size(); ++k)
    {
        size_t o = shape.size() - t.shape.size() + k;
        r[o] = t.shape[k] == 1 ? 0 : s[k];
    }
    return r;
}

template <typename F>
Tensor elementwise (const std::vector<const Tensor*>& ins, F fn)
{
    auto shape = broadcastShape (ins);
    Tensor out = Tensor::floats (shape);
    const size_t n = out.size(), rank = shape.size();
    std::vector<std::vector<int64_t>> st;
    std::vector<std::vector<float>> data;
    for (auto* t : ins)
    {
        st.push_back (bstrides (*t, shape));
        if (t->isInt) { std::vector<float> d (t->i.begin(), t->i.end()); data.push_back (std::move (d)); }
        else data.push_back (t->f);
    }
    std::vector<int64_t> idx (rank, 0);
    std::vector<int64_t> off (ins.size(), 0);
    float args[3];
    for (size_t e = 0; e < n; ++e)
    {
        for (size_t a = 0; a < ins.size(); ++a) args[a] = data[a][(size_t) off[a]];
        out.f[e] = fn (args);
        // increment the multi-index
        for (int k = (int) rank - 1; k >= 0; --k)
        {
            ++idx[(size_t) k];
            for (size_t a = 0; a < ins.size(); ++a) off[a] += st[a][(size_t) k];
            if (idx[(size_t) k] < shape[(size_t) k]) break;
            for (size_t a = 0; a < ins.size(); ++a) off[a] -= st[a][(size_t) k] * shape[(size_t) k];
            idx[(size_t) k] = 0;
        }
    }
    return out;
}

Tensor reduce (const Tensor& x, std::vector<int64_t> axes, bool keep, int kind)   // 0 sum, 1 min, 2 max
{
    const size_t rank = x.shape.size();
    std::vector<bool> red (rank, axes.empty());
    for (auto a : axes) red[(size_t) normAxis (a, rank)] = true;
    std::vector<int64_t> keptShape (rank);
    for (size_t k = 0; k < rank; ++k) keptShape[k] = red[k] ? 1 : x.shape[k];
    Tensor out = Tensor::floats (keptShape);
    const float init = kind == 0 ? 0.0f : (kind == 1 ? INFINITY : -INFINITY);
    std::fill (out.f.begin(), out.f.end(), init);
    auto xs = stridesOf (x.shape), os = stridesOf (keptShape);
    std::vector<int64_t> idx (rank, 0);
    for (size_t e = 0; e < x.f.size(); ++e)
    {
        int64_t o = 0;
        for (size_t k = 0; k < rank; ++k) if (! red[k]) o += idx[k] * os[k];
        float& r = out.f[(size_t) o];
        const float v = x.f[e];
        r = kind == 0 ? r + v : (kind == 1 ? std::min (r, v) : std::max (r, v));
        for (int k = (int) rank - 1; k >= 0; --k)
        {
            if (++idx[(size_t) k] < x.shape[(size_t) k]) break;
            idx[(size_t) k] = 0;
        }
    }
    if (! keep)
    {
        std::vector<int64_t> s;
        for (size_t k = 0; k < rank; ++k) if (! red[k]) s.push_back (x.shape[k]);
        out.shape = s;
    }
    return out;
}

Tensor conv (const Tensor& x, const Tensor& w, const Tensor* b, std::vector<int64_t> strides,
             std::vector<int64_t> pads, std::vector<int64_t> dil, int64_t group)
{
    if (x.shape.size() != 4 || w.shape.size() != 4) throw std::runtime_error ("Conv: only 2-D supported");
    const int64_t N = x.shape[0], C = x.shape[1], H = x.shape[2], W = x.shape[3];
    const int64_t M = w.shape[0], Cg = w.shape[1], KH = w.shape[2], KW = w.shape[3];
    if (strides.empty()) strides = { 1, 1 };
    if (dil.empty()) dil = { 1, 1 };
    if (pads.empty()) pads = { 0, 0, 0, 0 };
    const int64_t pt = pads[0], pl = pads[1], pb = pads[2], pr = pads[3];
    const int64_t OH = (H + pt + pb - dil[0] * (KH - 1) - 1) / strides[0] + 1;
    const int64_t OW = (W + pl + pr - dil[1] * (KW - 1) - 1) / strides[1] + 1;
    const int64_t Mg = M / group;
    if (Cg * group != C) throw std::runtime_error ("Conv: channel mismatch");
    Tensor y = Tensor::floats ({ N, M, OH, OW });
    for (int64_t n = 0; n < N; ++n)
        for (int64_t m = 0; m < M; ++m)
        {
            float* yo = &y.f[(size_t) (((n * M + m) * OH) * OW)];
            const float bias = b ? b->f[(size_t) m] : 0.0f;
            std::fill (yo, yo + OH * OW, bias);
            const int64_t g = m / Mg;
            for (int64_t cg = 0; cg < Cg; ++cg)
            {
                const int64_t c = g * Cg + cg;
                const float* xi = &x.f[(size_t) (((n * C + c) * H) * W)];
                for (int64_t kh = 0; kh < KH; ++kh)
                    for (int64_t kw = 0; kw < KW; ++kw)
                    {
                        const float wv = w.f[(size_t) (((m * Cg + cg) * KH + kh) * KW + kw)];
                        if (wv == 0.0f) continue;
                        for (int64_t oh = 0; oh < OH; ++oh)
                        {
                            const int64_t ih = oh * strides[0] - pt + kh * dil[0];
                            if (ih < 0 || ih >= H) continue;
                            const float* row = xi + ih * W;
                            float* yr = yo + oh * OW;
                            const int64_t s = strides[1], base = kw * dil[1] - pl;
                            // valid ow range: 0 <= ow*s + base < W
                            int64_t ow0 = base >= 0 ? 0 : (-base + s - 1) / s;
                            int64_t ow1 = std::min (OW, (W - base + s - 1) / s);
                            for (int64_t ow = ow0; ow < ow1; ++ow) yr[ow] += wv * row[ow * s + base];
                        }
                    }
            }
        }
    return y;
}

Tensor pad (const Tensor& x, const std::vector<int64_t>& pads, const std::string& mode, float value)
{
    const size_t rank = x.shape.size();
    std::vector<int64_t> os (rank);
    for (size_t k = 0; k < rank; ++k) os[k] = x.shape[k] + pads[k] + pads[k + rank];
    Tensor y = Tensor::floats (os);
    auto xs = stridesOf (x.shape);
    std::vector<int64_t> idx (rank, 0);
    const bool reflect = mode == "reflect", edge = mode == "edge";
    for (size_t e = 0; e < y.f.size(); ++e)
    {
        int64_t off = 0; bool inside = true;
        for (size_t k = 0; k < rank; ++k)
        {
            int64_t i = idx[k] - pads[k];
            const int64_t d = x.shape[k];
            if (i < 0 || i >= d)
            {
                if (reflect) { while (i < 0 || i >= d) i = i < 0 ? -i : 2 * (d - 1) - i; }
                else if (edge) i = std::clamp<int64_t> (i, 0, d - 1);
                else { inside = false; break; }
            }
            off += i * xs[k];
        }
        y.f[e] = inside ? x.f[(size_t) off] : value;
        for (int k = (int) rank - 1; k >= 0; --k)
        {
            if (++idx[(size_t) k] < os[(size_t) k]) break;
            idx[(size_t) k] = 0;
        }
    }
    return y;
}

Tensor transpose (const Tensor& x, std::vector<int64_t> perm)
{
    const size_t rank = x.shape.size();
    if (perm.empty()) { perm.resize (rank); for (size_t k = 0; k < rank; ++k) perm[k] = (int64_t) (rank - 1 - k); }
    std::vector<int64_t> os (rank);
    for (size_t k = 0; k < rank; ++k) os[k] = x.shape[(size_t) perm[k]];
    Tensor y = Tensor::floats (os);
    auto xs = stridesOf (x.shape);
    std::vector<int64_t> idx (rank, 0);
    for (size_t e = 0; e < y.f.size(); ++e)
    {
        int64_t off = 0;
        for (size_t k = 0; k < rank; ++k) off += idx[k] * xs[(size_t) perm[k]];
        y.f[e] = x.f[(size_t) off];
        for (int k = (int) rank - 1; k >= 0; --k)
        {
            if (++idx[(size_t) k] < os[(size_t) k]) break;
            idx[(size_t) k] = 0;
        }
    }
    return y;
}

Tensor slice (const Tensor& x, const std::vector<int64_t>& starts, const std::vector<int64_t>& ends,
              std::vector<int64_t> axes, std::vector<int64_t> steps)
{
    const size_t rank = x.shape.size();
    if (axes.empty()) { axes.resize (starts.size()); std::iota (axes.begin(), axes.end(), 0); }
    if (steps.empty()) steps.assign (starts.size(), 1);
    std::vector<int64_t> st (rank, 0), sp (rank, 1), os = x.shape;
    for (size_t j = 0; j < axes.size(); ++j)
    {
        const int a = normAxis (axes[j], rank);
        const int64_t d = x.shape[(size_t) a], step = steps[j];
        int64_t s = starts[j], e = ends[j];
        if (s < 0) s += d;
        if (e < 0) e += d;
        if (step > 0) { s = std::clamp<int64_t> (s, 0, d); e = std::clamp<int64_t> (e, 0, d); os[(size_t) a] = std::max<int64_t> (0, (e - s + step - 1) / step); }
        else { s = std::clamp<int64_t> (s, 0, d - 1); e = std::clamp<int64_t> (e, -1, d - 1); os[(size_t) a] = std::max<int64_t> (0, (s - e + (-step) - 1) / (-step)); }
        st[(size_t) a] = s; sp[(size_t) a] = step;
    }
    Tensor y = Tensor::floats (os);
    auto xs = stridesOf (x.shape);
    std::vector<int64_t> idx (rank, 0);
    for (size_t e = 0; e < y.f.size(); ++e)
    {
        int64_t off = 0;
        for (size_t k = 0; k < rank; ++k) off += (st[k] + idx[k] * sp[k]) * xs[k];
        y.f[e] = x.f[(size_t) off];
        for (int k = (int) rank - 1; k >= 0; --k)
        {
            if (++idx[(size_t) k] < os[(size_t) k]) break;
            idx[(size_t) k] = 0;
        }
    }
    return y;
}

Tensor concat (const std::vector<const Tensor*>& ts, int64_t axis)
{
    const size_t rank = ts[0]->shape.size();
    const int a = normAxis (axis, rank);
    auto os = ts[0]->shape;
    os[(size_t) a] = 0;
    for (auto* t : ts) os[(size_t) a] += t->shape[(size_t) a];
    Tensor y = Tensor::floats (os);
    int64_t outer = 1, inner = 1;
    for (int k = 0; k < a; ++k) outer *= os[(size_t) k];
    for (size_t k = (size_t) a + 1; k < rank; ++k) inner *= os[k];
    size_t w = 0;
    for (int64_t o = 0; o < outer; ++o)
        for (auto* t : ts)
        {
            const int64_t n = t->shape[(size_t) a] * inner;
            std::memcpy (&y.f[w], &t->f[(size_t) (o * n)], sizeof (float) * (size_t) n);
            w += (size_t) n;
        }
    return y;
}
} // namespace

int64_t GraphRunner::Node::attrInt (const char* name, int64_t def) const
{
    auto it = attrs.find (name);
    return it == attrs.end() ? def : it->second.i;
}
std::vector<int64_t> GraphRunner::Node::attrInts (const char* name) const
{
    auto it = attrs.find (name);
    return it == attrs.end() ? std::vector<int64_t> {} : it->second.ints;
}
std::string GraphRunner::Node::attrStr (const char* name) const
{
    auto it = attrs.find (name);
    return it == attrs.end() ? std::string {} : it->second.s;
}

bool GraphRunner::load (const void* data, size_t size)
{
    loaded = false;
    Reader r { (const uint8_t*) data, (const uint8_t*) data + size };
    if (size < 4 || std::memcmp (data, "BPG1", 4) != 0) { err = "bad magic"; return false; }
    r.p += 4;
    std::unordered_map<std::string, int> slot;
    auto slotOf = [&] (const std::string& n) -> int {
        if (n.empty()) return -1;
        auto it = slot.find (n);
        if (it != slot.end()) return it->second;
        slot[n] = nSlots;
        constants.emplace_back();
        isConst.push_back (false);
        return nSlots++;
    };
    constants.clear(); isConst.clear(); nodes.clear(); nSlots = 0;

    const auto nT = r.get<uint32_t>();
    for (uint32_t k = 0; k < nT && r.ok; ++k)
    {
        auto name = r.str();
        const auto dtype = r.get<uint8_t>();
        const auto nd = r.get<uint8_t>();
        Tensor t;
        for (int d = 0; d < nd; ++d) t.shape.push_back (r.get<int64_t>());
        const size_t n = t.size();
        const size_t bytes = n * (dtype == 0 ? 4 : 8);
        if ((size_t) (r.end - r.p) < bytes) { r.ok = false; break; }
        if (dtype == 0) { t.f.resize (n); std::memcpy (t.f.data(), r.p, bytes); }
        else { t.isInt = true; t.i.resize (n); std::memcpy (t.i.data(), r.p, bytes); }
        r.p += bytes;
        const int s = slotOf (name);
        constants[(size_t) s] = std::move (t);
        isConst[(size_t) s] = true;
    }
    const auto nN = r.get<uint32_t>();
    for (uint32_t k = 0; k < nN && r.ok; ++k)
    {
        Node nd;
        nd.op = r.str();
        const auto ni = r.get<uint8_t>();
        for (int j = 0; j < ni; ++j) nd.in.push_back (slotOf (r.str()));
        const auto no = r.get<uint8_t>();
        for (int j = 0; j < no; ++j) nd.out.push_back (slotOf (r.str()));
        const auto na = r.get<uint8_t>();
        for (int j = 0; j < na; ++j)
        {
            auto name = r.str();
            Attr a;
            a.kind = r.get<uint8_t>();
            if (a.kind == 0) a.i = r.get<int64_t>();
            else if (a.kind == 1) { const auto c = r.get<uint8_t>(); for (int q = 0; q < c; ++q) a.ints.push_back (r.get<int64_t>()); }
            else if (a.kind == 2) a.f = r.get<float>();
            else a.s = r.str();
            nd.attrs[name] = a;
        }
        nodes.push_back (std::move (nd));
    }
    input = r.str();
    inputSlot = slotOf (input);
    const auto nOut = r.get<uint8_t>();
    outputs.clear(); outputSlots.clear();
    for (int j = 0; j < nOut; ++j) { outputs.push_back (r.str()); outputSlots.push_back (slotOf (outputs.back())); }
    if (! r.ok) { err = "truncated graph file"; return false; }
    loaded = true;
    return true;
}

std::vector<Tensor> GraphRunner::run (const Tensor& in) const
{
    if (! loaded) throw std::runtime_error ("graph not loaded");
    std::vector<Tensor> v ((size_t) nSlots);
    std::vector<const Tensor*> ref ((size_t) nSlots, nullptr);
    for (int s = 0; s < nSlots; ++s) if (isConst[(size_t) s]) ref[(size_t) s] = &constants[(size_t) s];
    v[(size_t) inputSlot] = in;
    ref[(size_t) inputSlot] = &v[(size_t) inputSlot];

    // last use of every slot, so intermediate tensors are freed early
    std::vector<int> lastUse ((size_t) nSlots, -1);
    for (int k = 0; k < (int) nodes.size(); ++k)
        for (int s : nodes[(size_t) k].in) if (s >= 0) lastUse[(size_t) s] = k;
    for (int s : outputSlots) lastUse[(size_t) s] = (int) nodes.size();

    for (int k = 0; k < (int) nodes.size(); ++k)
    {
        const auto& n = nodes[(size_t) k];
        auto I = [&] (size_t j) -> const Tensor& {
            if (j >= n.in.size() || n.in[j] < 0 || ! ref[(size_t) n.in[j]]) throw std::runtime_error ("missing input for " + n.op);
            return *ref[(size_t) n.in[j]];
        };
        auto has = [&] (size_t j) { return j < n.in.size() && n.in[j] >= 0; };
        Tensor y;
        const auto& op = n.op;
        if (op == "Conv")
            y = conv (I (0), I (1), has (2) ? &I (2) : nullptr, n.attrInts ("strides"), n.attrInts ("pads"),
                      n.attrInts ("dilations"), n.attrInt ("group", 1));
        else if (op == "Reshape")
        {
            const auto& x = I (0);
            auto s = toInts (I (1));
            int64_t known = 1; int neg = -1;
            for (size_t j = 0; j < s.size(); ++j)
            {
                if (s[j] == 0) s[j] = x.shape[j];
                if (s[j] == -1) neg = (int) j; else known *= s[j];
            }
            if (neg >= 0) s[(size_t) neg] = (int64_t) x.size() / known;
            y = x; y.shape = s;
        }
        else if (op == "Unsqueeze")
        {
            const auto& x = I (0);
            auto axes = has (1) ? toInts (I (1)) : n.attrInts ("axes");
            const size_t rank = x.shape.size() + axes.size();
            std::vector<bool> ins (rank, false);
            for (auto a : axes) ins[(size_t) normAxis (a, rank)] = true;
            std::vector<int64_t> s; size_t q = 0;
            for (size_t j = 0; j < rank; ++j) s.push_back (ins[j] ? 1 : x.shape[q++]);
            y = x; y.shape = s;
        }
        else if (op == "Transpose") y = transpose (I (0), n.attrInts ("perm"));
        else if (op == "Slice")
            y = slice (I (0), toInts (I (1)), toInts (I (2)), has (3) ? toInts (I (3)) : std::vector<int64_t> {},
                       has (4) ? toInts (I (4)) : std::vector<int64_t> {});
        else if (op == "Pad")
        {
            const float cv = has (2) ? I (2).f.empty() ? 0.0f : I (2).f[0] : 0.0f;
            auto mode = n.attrStr ("mode");
            y = pad (I (0), toInts (I (1)), mode.empty() ? "constant" : mode, cv);
        }
        else if (op == "Concat")
        {
            std::vector<const Tensor*> ts;
            for (size_t j = 0; j < n.in.size(); ++j) ts.push_back (&I (j));
            y = concat (ts, n.attrInt ("axis", 0));
        }
        else if (op == "Neg")     y = elementwise ({ &I (0) }, [] (const float* a) { return -a[0]; });
        else if (op == "Sqrt")    y = elementwise ({ &I (0) }, [] (const float* a) { return std::sqrt (a[0]); });
        else if (op == "Log")     y = elementwise ({ &I (0) }, [] (const float* a) { return std::log (a[0]); });
        else if (op == "Relu")    y = elementwise ({ &I (0) }, [] (const float* a) { return a[0] > 0.0f ? a[0] : 0.0f; });
        else if (op == "Sigmoid") y = elementwise ({ &I (0) }, [] (const float* a) { return 1.0f / (1.0f + std::exp (-a[0])); });
        else if (op == "Add")     y = elementwise ({ &I (0), &I (1) }, [] (const float* a) { return a[0] + a[1]; });
        else if (op == "Sub")     y = elementwise ({ &I (0), &I (1) }, [] (const float* a) { return a[0] - a[1]; });
        else if (op == "Mul")     y = elementwise ({ &I (0), &I (1) }, [] (const float* a) { return a[0] * a[1]; });
        else if (op == "Div")     y = elementwise ({ &I (0), &I (1) }, [] (const float* a) { return a[0] / a[1]; });
        else if (op == "Equal")   y = elementwise ({ &I (0), &I (1) }, [] (const float* a) { return a[0] == a[1] ? 1.0f : 0.0f; });
        else if (op == "Where")   y = elementwise ({ &I (0), &I (1), &I (2) }, [] (const float* a) { return a[0] != 0.0f ? a[1] : a[2]; });
        else if (op == "ReduceSum" || op == "ReduceMin" || op == "ReduceMax")
        {
            std::vector<int64_t> axes = has (1) ? toInts (I (1)) : n.attrInts ("axes");
            if (axes.empty() && op == "ReduceSum" && n.attrInt ("noop_with_empty_axes", 0)) y = I (0);
            else y = reduce (I (0), axes, n.attrInt ("keepdims", 1) != 0, op == "ReduceSum" ? 0 : (op == "ReduceMin" ? 1 : 2));
        }
        else throw std::runtime_error ("unsupported op " + op);

        const int o = n.out[0];
        v[(size_t) o] = std::move (y);
        ref[(size_t) o] = &v[(size_t) o];
        for (int s : n.in)
            if (s >= 0 && ! isConst[(size_t) s] && lastUse[(size_t) s] == k) { v[(size_t) s] = Tensor(); ref[(size_t) s] = nullptr; }
    }
    std::vector<Tensor> out;
    for (int s : outputSlots) out.push_back (v[(size_t) s]);
    return out;
}
} // namespace ss
