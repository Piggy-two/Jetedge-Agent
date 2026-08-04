#!/usr/bin/env python3
"""Extract INT8 calibration frames from the four Stage 5 scene videos.

Usage:
    python3 scripts/extract_calib_frames.py [--out-dir /home/seeed/jetedge-calib]

Preprocessing matches the nvinfer chain (Stage 4/5 evidence): each source
frame is resized non-uniformly to the model input 640x384 (INTER_LINEAR,
approximating gstreamer bilinear scaling — a documented assumption) and
converted BGR -> RGB.  No letterbox, no normalization (the calibrator
applies the 1/255 factor like nvinfer's net-scale-factor).

Frames are written as raw RGB (HWC, uint8, 640*384*3 = 737280 B per file)
so the C++ calibrator can read them with fstream only (no OpenCV
dependency).  Frame numbers are 1-based to match the JSONL frame_num.

Output layout:
    <out-dir>/raw/<scene>_f<k>.rgb
    <out-dir>/manifest.json

Scenes (stride picked so each lands in ~140-190 frames):
    bus    sample_720p.h264       stride 8   (~180 of 1440)
    office sample_office.mp4      stride 1   (all 163)
    walk   sample_walk.mov        stride 2   (~144 of 288)
    ride   sample_ride_bike.mov   stride 1   (all 179)
Total ~666 frames (~490 MB).

--bus-stride / --walk-stride override the strides (used for the Stage 13
calibration-density experiment: bus 4 and walk 1 give ~990 frames).
"""

import argparse
import json
import os

import cv2

MODEL_W, MODEL_H = 640, 384
FRAME_BYTES = MODEL_W * MODEL_H * 3

SCENES = [
    ("bus", "sample_720p.h264", 8),
    ("office", "sample_office.mp4", 1),
    ("walk", "sample_walk.mov", 2),
    ("ride", "sample_ride_bike.mov", 1),
]

STREAMS_ROOT = "/opt/nvidia/deepstream/deepstream-7.1/samples/streams/"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", default="/home/seeed/jetedge-calib")
    parser.add_argument("--bus-stride", type=int, default=8)
    parser.add_argument("--walk-stride", type=int, default=2)
    args = parser.parse_args()

    overrides = {"bus": args.bus_stride, "walk": args.walk_stride}
    raw_dir = os.path.join(args.out_dir, "raw")
    os.makedirs(raw_dir, exist_ok=True)

    manifest: list[dict] = []
    total = 0
    for scene, video, stride in SCENES:
        stride = overrides.get(scene, stride)
        path = os.path.join(STREAMS_ROOT, video)
        cap = cv2.VideoCapture(path)
        if not cap.isOpened():
            raise SystemExit(f"cannot open video: {path}")
        scene_count = 0
        k = 0  # 1-based frame number, aligned with JSONL frame_num
        while True:
            ok, bgr = cap.read()
            if not ok:
                break
            k += 1
            if (k - 1) % stride != 0:
                continue
            rgb = cv2.cvtColor(cv2.resize(bgr, (MODEL_W, MODEL_H),
                                          interpolation=cv2.INTER_LINEAR),
                               cv2.COLOR_BGR2RGB)
            name = f"{scene}_f{k}.rgb"
            with open(os.path.join(raw_dir, name), "wb") as fh:
                fh.write(rgb.tobytes())
            manifest.append({"scene": scene, "video": video, "frame_num": k,
                             "file": name, "bytes": FRAME_BYTES})
            scene_count += 1
            total += 1
        cap.release()
        print(f"{scene:<8} {scene_count:>4} frames (stride {stride})")

    with open(os.path.join(args.out_dir, "manifest.json"), "w",
              encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2)

    print(f"total {total} frames -> {raw_dir}")
    if total != len(manifest):
        raise SystemExit("manifest length mismatch")
    print(f"manifest: {os.path.join(args.out_dir, 'manifest.json')} "
          f"({len(manifest)} entries)")


if __name__ == "__main__":
    main()
