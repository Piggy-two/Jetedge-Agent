#!/usr/bin/env python3
"""Pure logic for the Stage 13 INT8 accuracy regression.

Shared by scripts/run_ort_reference.py (decode + NMS) and
scripts/compare_precision.py (matching + statistics).  No external
dependencies beyond numpy, so the unit tests in scripts/tests/
run anywhere.

Detection convention used throughout:
    det = {"class_id": int, "confidence": float,
           "x1": float, "y1": float, "x2": float, "y2": float}
in absolute pixel coordinates of the frame space they were decoded in
(network 640x384 space, or source-frame space after scaling back).

The YOLO11 decode replicates src/inference/yolo11_parser.cpp semantics:
  output float[1][84][5040]; channels 0..3 are (cx, cy, w, h) as ABSOLUTE
  pixel coordinates in the model input space (already decoded by the graph),
  channels 4..83 are class scores ALREADY SIGMOIDED to [0, 1].
"""

from __future__ import annotations

import math

import numpy as np

# Model input resolution (accepted ONNX export spec).
MODEL_W, MODEL_H = 640, 384
NUM_CLASSES = 80


def xywh_to_xyxy(box) -> tuple[float, float, float, float]:
    """[left, top, width, height] -> (x1, y1, x2, y2)."""
    left, top, width, height = box
    return (left, top, left + width, top + height)


def iou(a, b) -> float:
    """Intersection-over-union of two xyxy boxes."""
    ax1, ay1, ax2, ay2 = a
    bx1, by1, bx2, by2 = b
    ix1 = max(ax1, bx1)
    iy1 = max(ay1, by1)
    ix2 = min(ax2, bx2)
    iy2 = min(ay2, by2)
    if ix2 <= ix1 or iy2 <= iy1:
        return 0.0
    inter = (ix2 - ix1) * (iy2 - iy1)
    area_a = max(ax2 - ax1, 0.0) * max(ay2 - ay1, 0.0)
    area_b = max(bx2 - bx1, 0.0) * max(by2 - by1, 0.0)
    union = area_a + area_b - inter
    if union <= 0.0:
        return 0.0
    return inter / union


def greedy_match(dets_a: list[dict], dets_b: list[dict],
                 iou_th: float = 0.5) -> tuple[list[tuple], list[int], list[int]]:
    """Greedy IoU matching, class-agnostic (a class flip shows up as
    unmatched + extra instead of being silently accepted).

    A detections are processed in descending confidence order; each one is
    paired with the highest-IoU still-unmatched B detection (>= iou_th).
    Returns (matches, unmatched_a, unmatched_b) where matches is a list of
    (idx_a, idx_b, iou_value).
    """
    order_a = sorted(range(len(dets_a)), key=lambda i: dets_a[i]["confidence"],
                     reverse=True)
    used_b: set[int] = set()
    matches: list[tuple] = []
    for ia in order_a:
        best_jb = -1
        best_iou = iou_th
        for ib in range(len(dets_b)):
            if ib in used_b:
                continue
            v = iou(box_of(dets_a[ia]), box_of(dets_b[ib]))
            if v > best_iou:
                best_iou = v
                best_jb = ib
        if best_jb >= 0:
            used_b.add(best_jb)
            matches.append((ia, best_jb, best_iou))
    unmatched_a = [i for i in range(len(dets_a)) if i not in {m[0] for m in matches}]
    unmatched_b = [i for i in range(len(dets_b)) if i not in {m[1] for m in matches}]
    return matches, unmatched_a, unmatched_b


def nms_per_class(dets: list[dict], iou_th: float = 0.5,
                  topk: int = 20) -> list[dict]:
    """Per-class NMS approximating nvinfer cluster-mode=2 semantics:
    detections below the pre-cluster threshold were already filtered by
    decode(); here classes are suppressed independently, highest confidence
    first, keeping at most `topk` per class."""
    by_class: dict[int, list[int]] = {}
    for i, d in enumerate(dets):
        by_class.setdefault(d["class_id"], []).append(i)
    kept: list[dict] = []
    for cls, idxs in by_class.items():
        idxs.sort(key=lambda i: dets[i]["confidence"], reverse=True)
        pick: list[int] = []
        for i in idxs:
            suppressed = False
            for j in pick:
                if iou(box_of(dets[i]), box_of(dets[j])) > iou_th:
                    suppressed = True
                    break
            if not suppressed:
                pick.append(i)
        for i in pick[:topk]:
            kept.append(dets[i])
    return kept


def decode_yolo11(output: np.ndarray, conf_th: float = 0.25,
                  num_classes: int = NUM_CLASSES,
                  w: int = MODEL_W, h: int = MODEL_H) -> list[dict]:
    """Decode a 1x84x5040 output tensor into detections in model-input
    space, replicating src/inference/yolo11_parser.cpp:
      ch0..3 per cell: cx, cy, w, h (absolute pixels, already decoded)
      ch4..83 per cell: sigmoided class scores
    Filters: best score >= conf_th, w > 0, h > 0; boxes clipped to the
    network resolution (right<=left or bottom<=top drops the cell)."""
    out = np.asarray(output, dtype=np.float32)
    if out.ndim != 3 or out.shape[0] != 1:
        raise ValueError(f"unexpected output shape {out.shape}")
    if out.shape[1] < num_classes + 4:
        raise ValueError(f"unexpected channel count {out.shape[1]}")
    cells = out.shape[2]
    cx = out[0, 0, :]
    cy = out[0, 1, :]
    bw = out[0, 2, :]
    bh = out[0, 3, :]
    scores = out[0, 4:4 + num_classes, :]  # num_classes x cells

    best_cls = np.argmax(scores, axis=0)
    best_score = scores[best_cls, np.arange(cells)]

    valid = (best_score >= conf_th) & (bw > 0.0) & (bh > 0.0)
    idx = np.nonzero(valid)[0]

    dets: list[dict] = []
    for i in idx:
        left = float(min(max(cx[i] - bw[i] * 0.5, 0.0), w - 1.0))
        top = float(min(max(cy[i] - bh[i] * 0.5, 0.0), h - 1.0))
        right = float(min(max(cx[i] + bw[i] * 0.5, 0.0), w - 1.0))
        bottom = float(min(max(cy[i] + bh[i] * 0.5, 0.0), h - 1.0))
        if right <= left or bottom <= top:
            continue
        dets.append({"class_id": int(best_cls[i]),
                     "confidence": float(best_score[i]),
                     "x1": left, "y1": top, "x2": right, "y2": bottom})
    return dets


def box_of(det: dict) -> tuple[float, float, float, float]:
    return (det["x1"], det["y1"], det["x2"], det["y2"])


# ---- Statistics -------------------------------------------------------------

def pct(xs: list[float], p: float) -> float:
    """Nearest-rank percentile (p in [0, 100])."""
    if not xs:
        return 0.0
    s = sorted(xs)
    idx = int(math.ceil(p / 100.0 * len(s))) - 1
    return s[max(idx, 0)]


def mean(xs: list[float]) -> float:
    return sum(xs) / len(xs) if xs else 0.0


def match_rate(matches: list[tuple], total: int) -> float:
    """Fraction of the total detections that found a partner."""
    return len(matches) / total if total else 0.0


def conf_deltas(matches: list[tuple], dets_a: list[dict],
                dets_b: list[dict]) -> list[float]:
    return [abs(dets_a[ia]["confidence"] - dets_b[ib]["confidence"])
            for ia, ib, _ in matches]


def class_consistency(matches: list[tuple], dets_a: list[dict],
                      dets_b: list[dict]) -> float:
    if not matches:
        return 1.0
    same = sum(1 for ia, ib, _ in matches
               if dets_a[ia]["class_id"] == dets_b[ib]["class_id"])
    return same / len(matches)
