// A tiny inference engine for small, static-shape ONNX graphs that were converted to the
// "BPG1" format by tools/export_basic_pitch.py. It supports only the operators that the
// Basic Pitch graph uses (Conv, Pad, Reshape, Transpose, Slice, Concat, element-wise maths and
// a few reductions) and runs them in plain scalar C++, so no ONNX Runtime is needed.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ss
{
struct Tensor
{
    std::vector<int64_t> shape;
    std::vector<float> f;      // float data (also used for booleans: 0 / 1)
    std::vector<int64_t> i;    // int64 data (shape-like constants)
    bool isInt = false;

    size_t size() const;
    static Tensor floats (std::vector<int64_t> shape);
};

class GraphRunner
{
public:
    // Loads a BPG1 graph from memory. Returns false (and sets error()) on a malformed file.
    bool load (const void* data, size_t size);
    bool isLoaded() const { return loaded; }
    const std::string& error() const { return err; }

    const std::string& inputName() const { return input; }
    const std::vector<std::string>& outputNames() const { return outputs; }

    // Runs the graph. Throws std::runtime_error on an unsupported op or a shape problem.
    std::vector<Tensor> run (const Tensor& in) const;

private:
    struct Attr { int kind = 0; int64_t i = 0; float f = 0; std::vector<int64_t> ints; std::string s; };
    struct Node
    {
        std::string op;
        std::vector<int> in, out;   // value slots (-1 = absent optional input)
        std::map<std::string, Attr> attrs;
        int64_t attrInt (const char* name, int64_t def) const;
        std::vector<int64_t> attrInts (const char* name) const;
        std::string attrStr (const char* name) const;
    };

    std::vector<Tensor> constants;           // slot -> constant (initialisers)
    std::vector<bool> isConst;
    std::vector<Node> nodes;
    int nSlots = 0, inputSlot = -1;
    std::vector<int> outputSlots;
    std::string input, err;
    std::vector<std::string> outputs;
    bool loaded = false;
};
} // namespace ss
