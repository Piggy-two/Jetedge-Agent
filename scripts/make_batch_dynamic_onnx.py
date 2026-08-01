#!/usr/bin/env python3
"""Derive a batch-dynamic ONNX from the accepted static batch=1 ONNX.

Changes (all batch-dimension related, weights unchanged):
  1. graph input/output batch dims are made symbolic ("batch");
  2. every Reshape target initializer that hard-codes batch=1 in its first
     element is converted to batch-relative: [1, ...] -> [0, ...] (ONNX
     Reshape: 0 copies the corresponding input dim).  Verified on the
     accepted yolo11s.onnx: C2PSA attention reshapes
     (/model.10/m/m.0/attn/Constant{,_3}_output_0), the DFL head reshape
     (/model.23/dfl/Constant_output_0) and the shared stride flatten reshape
     (/model.23/Constant_output_0 = [1,64,-1]).

No weights, operators, or other dimensions change.  The derived file is
intended for TensorRT engine builds with a batch profile (MIN=1 OPT=4 MAX=4).

Usage:
  python3 scripts/make_batch_dynamic_onnx.py <yolo11s.onnx> <yolo11s_dynamic.onnx>

Prints SHA256 of both files.
"""

import hashlib
import sys

import onnx
from onnx import shape_inference


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <input.onnx> <output.onnx>")
        return 1

    src, dst = sys.argv[1], sys.argv[2]
    model = onnx.load(src)
    onnx.checker.check_model(model)

    dynamic_dims = set()

    def make_batch_dynamic(vi) -> bool:
        if not vi.type.HasField("tensor_type"):
            return False
        shape = vi.type.tensor_type.shape
        if not shape.dim or not shape.dim[0].HasField("dim_value"):
            return False
        shape.dim[0].dim_param = "batch"
        shape.dim[0].ClearField("dim_value")
        return True

    # Inputs: make dim0 symbolic.
    for vi in model.graph.input:
        if make_batch_dynamic(vi):
            dynamic_dims.add(vi.name)

    # Outputs: make dim0 symbolic (the detection head scales with batch).
    for vi in model.graph.output:
        if make_batch_dynamic(vi):
            dynamic_dims.add(vi.name)

    # Reshape target initializers with a hard-coded batch=1 first element
    # must become batch-relative (0 = copy from input) for batch>1 inference.
    reshape_shape_inits = {}
    for node in model.graph.node:
        if node.op_type == "Reshape" and len(node.input) >= 2:
            reshape_shape_inits.setdefault(node.input[1], []).append(node.name)

    fixed = []
    for init in model.graph.initializer:
        if init.name not in reshape_shape_inits:
            continue
        if init.data_type != onnx.TensorProto.INT64:
            raise SystemExit(f"unexpected dtype for reshape target {init.name}: {init.data_type}")
        if len(init.raw_data) < 8:
            raise SystemExit(f"empty reshape target {init.name}")
        first = int.from_bytes(init.raw_data[0:8], "little", signed=True)
        if first != 1:
            continue  # already batch-relative or fixed
        data = bytearray(init.raw_data)
        data[0:8] = (0).to_bytes(8, "little", signed=True)
        init.raw_data = bytes(data)
        fixed.append(init.name)

    if fixed:
        print("batch-relative reshape initializers fixed:")
        for name in fixed:
            used_by = ", ".join(reshape_shape_inits[name])
            print(f"  {name}  (used by {used_by})")

    onnx.checker.check_model(model)
    inferred = shape_inference.infer_shapes(model)
    onnx.save(inferred, dst)

    print(f"dynamic batch dims set for: {sorted(dynamic_dims)}")
    print(f"batch-relative reshape initializers fixed: {sorted(fixed)}")
    print(f"source: {src} sha256={sha256(src)}")
    print(f"derived: {dst} sha256={sha256(dst)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
