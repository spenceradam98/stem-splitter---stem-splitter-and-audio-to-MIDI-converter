#!/usr/bin/env python3
"""Converts Spotify's Basic Pitch ONNX model (Apache 2.0) to the small graph file read by
Source/engine/BasicPitch.cpp, so the plugin needs no ONNX Runtime.

    pip install basic-pitch==0.4.0 onnx onnxsim --no-deps   # (onnx, onnxsim need their own deps)
    python tools/export_basic_pitch.py <path to nmp.onnx> Source/engine/models/basic_pitch.bpg

The graph is simplified with a fixed input shape [1, 43844, 1] (one 2-second window at 22050 Hz),
so every shape is static. File layout (little endian):
    "BPG1"
    u32 nTensors; per tensor: str name, u8 dtype (0 f32, 1 i64), u8 ndim, i64 dims[ndim], data
    u32 nNodes;   per node:   str op, u8 nIn, str in[], u8 nOut, str out[], u8 nAttr,
                              per attr: str name, u8 kind (0 int, 1 ints, 2 float, 3 string), payload
    str input; u8 nOutputs; str outputs[]  (order: note, onset, contour)
where str = u16 length + bytes.
"""
import struct, sys
import numpy as np
import onnx, onnxsim
from onnx import numpy_helper

src, dst = sys.argv[1], sys.argv[2]
model = onnx.load(src)
inp = model.graph.input[0].name
model, ok = onnxsim.simplify(model, overwrite_input_shapes={inp: [1, 43844, 1]})
assert ok
g = model.graph

out = bytearray(b"BPG1")
def s(x):
    b = x.encode() if isinstance(x, str) else bytes(x)
    out.extend(struct.pack("<H", len(b))); out.extend(b)

out.extend(struct.pack("<I", len(g.initializer)))
for t in g.initializer:
    a = numpy_helper.to_array(t)
    s(t.name)
    if a.dtype == np.float32:  out.extend(struct.pack("<B", 0))
    elif a.dtype == np.int64:  out.extend(struct.pack("<B", 1))
    else: raise SystemExit(f"unsupported dtype {a.dtype} in {t.name}")
    out.extend(struct.pack("<B", a.ndim))
    for d in a.shape: out.extend(struct.pack("<q", d))
    out.extend(np.ascontiguousarray(a).astype(a.dtype.newbyteorder("<")).tobytes())

out.extend(struct.pack("<I", len(g.node)))
for n in g.node:
    s(n.op_type)
    out.extend(struct.pack("<B", len(n.input)));  [s(x) for x in n.input]
    out.extend(struct.pack("<B", len(n.output))); [s(x) for x in n.output]
    out.extend(struct.pack("<B", len(n.attribute)))
    for a in n.attribute:
        v = onnx.helper.get_attribute_value(a)
        s(a.name)
        if isinstance(v, int):     out.extend(struct.pack("<Bq", 0, v))
        elif isinstance(v, float): out.extend(struct.pack("<Bf", 2, v))
        elif isinstance(v, bytes): out.extend(struct.pack("<B", 3)); s(v)
        elif isinstance(v, list) and all(isinstance(e, int) for e in v):
            out.extend(struct.pack("<BB", 1, len(v))); [out.extend(struct.pack("<q", e)) for e in v]
        else: raise SystemExit(f"unsupported attribute {a.name} on {n.op_type}")

s(inp)
names = {"StatefulPartitionedCall:1": 0, "StatefulPartitionedCall:2": 1, "StatefulPartitionedCall:0": 2}
outs = sorted([o.name for o in g.output], key=lambda k: names[k])
out.extend(struct.pack("<B", len(outs))); [s(o) for o in outs]
open(dst, "wb").write(out)
print(f"wrote {dst}: {len(out)} bytes, {len(g.node)} nodes, {len(g.initializer)} tensors")
