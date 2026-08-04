#!/usr/bin/env python3
"""Generate the ORT FP32 reference detections for the Stage 13 accuracy
regression.

Usage:
    python3 scripts/run_ort_reference.py [--out logs/ort_reference.jsonl]
            [--max-frames N]

Runs the accepted yolo11s.onnx (static batch=1, 1x3x384x640) in ONNX
Runtime CPU on every frame of the four Stage 5 scene videos and writes one
JSONL record per detection with the same fields as the pipeline output:
    {"stream_id", "frame_num", "class_id", "confidence", "bbox":[l,t,w,h]}

Preprocessing replicates the nvinfer chain (Stage 4/5 evidence):
source frame -> resize 1280x720 (nvstreammux) -> resize 640x384 (nvinfer),
both INTER_LINEAR (an approximation of gstreamer bilinear scaling — a
documented assumption).  BGR->RGB, /255.  Decoding replicates
src/inference/yolo11_parser.cpp semantics via accuracy_math.decode_yolo11,
NMS approximates cluster-mode=2 via accuracy_math.nms_per_class
(pre-cluster threshold 0.25, IoU 0.5, topk 20).  Boxes are scaled back to
source-frame coordinates (linear, non-uniform) to match the pipeline JSONL.

This reference is a SECONDARY comparison target: nvinfer's NMS clustering
differs in implementation details from the numpy NMS here, so the
FP16-vs-ORT protocol difference is the noise baseline for INT8-vs-ORT.
"""

import argparse
import json
import sys
import time

import cv2
import numpy as np
import onnxruntime as ort

sys.path.insert(0, __import__("os").path.dirname(__file__))
from accuracy_math import decode_yolo11, nms_per_class  # noqa: E402

# (stream_id, video, source resolution) — matches streams_stage13_*.yaml.
SCENES = [
    ("cam1", "sample_720p.h264", (1280, 720)),
    ("cam2", "sample_office.mp4", (1728, 1080)),
    ("cam3", "sample_walk.mov", (1920, 1080)),
    ("cam4", "sample_ride_bike.mov", (1920, 1080)),
]
STREAMS_ROOT = "/opt/nvidia/deepstream/deepstream-7.1/samples/streams/"
MUX_W, MUX_H = 1280, 720
MODEL_W, MODEL_H = 640, 384


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out",
                        default="/home/seeed/projects/jetedge-agent/logs/"
                                "ort_reference.jsonl")
    parser.add_argument("--max-frames", type=int, default=0,
                        help="cap frames per scene (0 = all)")
    args = parser.parse_args()

    sess = ort.InferenceSession(
        "/home/seeed/JetEdge-Agent/models/yolo11s.onnx",
        providers=["CPUExecutionProvider"])

    total = 0
    t0 = time.time()
    with open(args.out, "w", encoding="utf-8") as fh:
        for stream_id, video, (src_w, src_h) in SCENES:
            cap = cv2.VideoCapture(STREAMS_ROOT + video)
            if not cap.isOpened():
                raise SystemExit(f"cannot open video: {video}")
            scene_count = 0
            k = 0  # 1-based frame number, aligned with JSONL frame_num
            while True:
                ok, bgr = cap.read()
                if not ok:
                    break
                k += 1
                if args.max_frames and scene_count >= args.max_frames:
                    break
                # mux scale, then nvinfer scale (both INTER_LINEAR approx).
                mux = cv2.resize(bgr, (MUX_W, MUX_H),
                                 interpolation=cv2.INTER_LINEAR)
                net = cv2.resize(mux, (MODEL_W, MODEL_H),
                                 interpolation=cv2.INTER_LINEAR)
                rgb = cv2.cvtColor(net, cv2.COLOR_BGR2RGB)
                x = (rgb.astype(np.float32) / 255.0).transpose(2, 0, 1)
                x = x[np.newaxis, ...].astype(np.float32)  # 1x3x384x640
                out = sess.run(None, {"images": x})[0]      # 1x84x5040
                dets = nms_per_class(decode_yolo11(out))
                sx = src_w / MODEL_W
                sy = src_h / MODEL_H
                for d in dets:
                    left = d["x1"] * sx
                    top = d["y1"] * sy
                    right = d["x2"] * sx
                    bottom = d["y2"] * sy
                    fh.write(json.dumps({
                        "stream_id": stream_id,
                        "frame_num": k,
                        "class_id": d["class_id"],
                        "confidence": round(d["confidence"], 6),
                        "bbox": [round(left, 2), round(top, 2),
                                 round(right - left, 2),
                                 round(bottom - top, 2)],
                    }) + "\n")
                scene_count += 1
                total += 1
            cap.release()
            print(f"{stream_id:<6} {scene_count:>5} frames processed")
    print(f"total {total} frames -> {args.out} "
          f"({time.time() - t0:.1f} s)")


if __name__ == "__main__":
    main()
