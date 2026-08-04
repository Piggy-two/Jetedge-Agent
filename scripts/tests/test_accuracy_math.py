"""accuracy_math unit tests (Stage 13) — pure logic, no GStreamer/TRT."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from accuracy_math import (  # noqa: E402
    decode_yolo11,
    greedy_match,
    iou,
    nms_per_class,
    pct,
    xywh_to_xyxy,
)

D = lambda cls, conf, x1, y1, x2, y2: {  # noqa: E731
    "class_id": cls, "confidence": conf,
    "x1": x1, "y1": y1, "x2": x2, "y2": y2}


class IouTest(unittest.TestCase):
    def test_overlap(self):
        # A and B each area 100; intersection 50; union 150 -> IoU 1/3.
        self.assertAlmostEqual(iou((0, 0, 10, 10), (5, 0, 15, 10)), 1.0 / 3.0)

    def test_no_overlap(self):
        self.assertEqual(iou((0, 0, 10, 10), (20, 20, 30, 30)), 0.0)

    def test_containment(self):
        self.assertAlmostEqual(iou((0, 0, 10, 10), (2, 2, 8, 8)), 0.36)

    def test_touching_edges_no_area(self):
        self.assertEqual(iou((0, 0, 10, 10), (10, 0, 20, 10)), 0.0)

    def test_degenerate_box(self):
        self.assertEqual(iou((0, 0, 0, 10), (0, 0, 10, 10)), 0.0)


class BoxTest(unittest.TestCase):
    def test_xywh_to_xyxy(self):
        self.assertEqual(xywh_to_xyxy([10, 20, 30, 40]), (10, 20, 40, 60))


class GreedyMatchTest(unittest.TestCase):
    def test_simple_match(self):
        a = [D(0, 0.9, 0, 0, 10, 10)]
        b = [D(0, 0.8, 0, 0, 10, 10)]
        matches, ua, ub = greedy_match(a, b)
        self.assertEqual(len(matches), 1)
        self.assertAlmostEqual(matches[0][2], 1.0)
        self.assertEqual(ua, [])
        self.assertEqual(ub, [])

    def test_highest_conf_first(self):
        # Both A dets overlap B0; the higher-confidence A det gets B0 and
        # the lower one stays unmatched (B0 is single-use).
        a = [D(0, 0.9, 0, 0, 10, 10), D(0, 0.5, 0, 0, 10, 10)]
        b = [D(0, 0.8, 0, 0, 10, 10)]
        matches, ua, ub = greedy_match(a, b)
        self.assertEqual(len(matches), 1)
        self.assertEqual(matches[0][0], 0)  # conf 0.9 matched
        self.assertEqual(ua, [1])           # conf 0.5 left over

    def test_class_flip_is_unmatched(self):
        # Same box but different class: matching is class-agnostic so the
        # pair still matches (a flip would show as BOTH extra in a
        # class-restricted scheme); here it matches by IoU.
        a = [D(5, 0.9, 0, 0, 10, 10)]
        b = [D(2, 0.8, 0, 0, 10, 10)]
        matches, ua, ub = greedy_match(a, b)
        self.assertEqual(len(matches), 1)

    def test_below_iou_threshold_unmatched(self):
        a = [D(0, 0.9, 0, 0, 10, 10)]
        b = [D(0, 0.8, 30, 30, 40, 40)]
        matches, ua, ub = greedy_match(a, b)
        self.assertEqual(matches, [])
        self.assertEqual(ua, [0])
        self.assertEqual(ub, [0])

    def test_swap_directions_rates(self):
        a = [D(0, 0.9, 0, 0, 10, 10), D(0, 0.8, 20, 20, 30, 30)]
        b = [D(0, 0.7, 0, 0, 10, 10)]
        matches, ua, ub = greedy_match(a, b)
        self.assertEqual(len(matches), 1)
        self.assertEqual(len(ua), 1)
        self.assertEqual(len(ub), 0)


class NmsTest(unittest.TestCase):
    def test_duplicates_suppressed(self):
        dets = [D(0, 0.9, 0, 0, 10, 10), D(0, 0.8, 0, 0, 10, 10)]
        kept = nms_per_class(dets)
        self.assertEqual(len(kept), 1)

    def test_different_classes_kept(self):
        dets = [D(0, 0.9, 0, 0, 10, 10), D(1, 0.8, 0, 0, 10, 10)]
        kept = nms_per_class(dets)
        self.assertEqual(len(kept), 2)

    def test_distinct_boxes_kept(self):
        dets = [D(0, 0.9, 0, 0, 10, 10), D(0, 0.8, 100, 100, 110, 110)]
        kept = nms_per_class(dets)
        self.assertEqual(len(kept), 2)

    def test_topk_cap(self):
        # Five non-overlapping boxes of the same class: NMS keeps all, topk
        # caps the per-class output at 2.
        dets = [D(0, 0.9 - i * 0.01, i * 100, 0, i * 100 + 10, 10)
                for i in range(5)]
        kept = nms_per_class(dets, topk=2)
        self.assertEqual(len(kept), 2)


class DecodeTest(unittest.TestCase):
    def _tensor(self):
        out = np.zeros((1, 84, 5040), dtype=np.float32)
        return out

    def test_decode_known_cell(self):
        # One cell (index 100) with a strong detection:
        # cx=320 cy=192 w=100 h=60, class 5 score 0.95.
        out = self._tensor()
        out[0, 0, 100] = 320.0   # cx
        out[0, 1, 100] = 192.0   # cy
        out[0, 2, 100] = 100.0   # w
        out[0, 3, 100] = 60.0    # h
        out[0, 4 + 5, 100] = 0.95
        dets = decode_yolo11(out, conf_th=0.25)
        self.assertEqual(len(dets), 1)
        d = dets[0]
        self.assertEqual(d["class_id"], 5)
        self.assertAlmostEqual(d["confidence"], 0.95, places=5)
        self.assertAlmostEqual(d["x1"], 270.0)
        self.assertAlmostEqual(d["y1"], 162.0)
        self.assertAlmostEqual(d["x2"], 370.0)
        self.assertAlmostEqual(d["y2"], 222.0)

    def test_best_class_is_argmax(self):
        out = self._tensor()
        out[0, 0, 7] = 320.0
        out[0, 1, 7] = 192.0
        out[0, 2, 7] = 10.0
        out[0, 3, 7] = 10.0
        out[0, 4 + 2, 7] = 0.7   # class 2
        out[0, 4 + 5, 7] = 0.9   # class 5 wins
        dets = decode_yolo11(out)
        self.assertEqual(len(dets), 1)
        self.assertEqual(dets[0]["class_id"], 5)

    def test_below_threshold_filtered(self):
        out = self._tensor()
        out[0, 0, 3] = 320.0
        out[0, 1, 3] = 192.0
        out[0, 2, 3] = 10.0
        out[0, 3, 3] = 10.0
        out[0, 4 + 0, 3] = 0.1   # below 0.25
        dets = decode_yolo11(out, conf_th=0.25)
        self.assertEqual(dets, [])

    def test_degenerate_size_filtered(self):
        out = self._tensor()
        out[0, 0, 9] = 320.0
        out[0, 1, 9] = 192.0
        out[0, 2, 9] = 0.0    # w == 0 -> dropped
        out[0, 3, 9] = 10.0
        out[0, 4 + 0, 9] = 0.9
        dets = decode_yolo11(out)
        self.assertEqual(dets, [])

    def test_clip_to_network_bounds(self):
        # Box extending past the right edge is clipped to 639.
        out = self._tensor()
        out[0, 0, 5] = 630.0
        out[0, 1, 5] = 192.0
        out[0, 2, 5] = 100.0
        out[0, 3, 5] = 10.0
        out[0, 4 + 0, 5] = 0.9
        dets = decode_yolo11(out)
        self.assertEqual(len(dets), 1)
        self.assertAlmostEqual(dets[0]["x2"], 639.0)
        self.assertGreater(dets[0]["x1"], 0.0)

    def test_bad_shape_rejected(self):
        with self.assertRaises(ValueError):
            decode_yolo11(np.zeros((2, 84, 5040), dtype=np.float32))


class StatTest(unittest.TestCase):
    def test_pct_nearest_rank(self):
        xs = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0]
        self.assertEqual(pct(xs, 50), 5.0)
        self.assertEqual(pct(xs, 95), 10.0)
        self.assertEqual(pct([], 95), 0.0)


if __name__ == "__main__":
    unittest.main()
