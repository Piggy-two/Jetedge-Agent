"""Goal parser unit tests (Stage 12)."""

from __future__ import annotations

import unittest

from agent.goal_parser import parse_goal


class GoalParserTest(unittest.TestCase):
    def test_chinese_full(self):
        goal = parse_goal("保证 cam1 推理 FPS 不低于 15,降低全局 P95 延迟")
        self.assertFalse(goal.unsupported)
        self.assertEqual(goal.cam_id, "cam1")
        self.assertEqual(goal.min_fps, 15.0)

    def test_chinese_no_number_uses_default(self):
        goal = parse_goal("保证 cam2 推理 FPS 不低于,降低全局 P95 延迟")
        self.assertFalse(goal.unsupported)
        self.assertEqual(goal.cam_id, "cam2")
        self.assertEqual(goal.min_fps, 15.0)  # default

    def test_chinese_variant_wording(self):
        goal = parse_goal("在 cam4 至少 12 fps 的情况下降低时延")
        self.assertFalse(goal.unsupported)
        self.assertEqual(goal.cam_id, "cam4")
        self.assertEqual(goal.min_fps, 12.0)

    def test_english(self):
        goal = parse_goal("keep cam1 inference FPS >= 20 while reducing global P95 latency")
        self.assertFalse(goal.unsupported)
        self.assertEqual(goal.cam_id, "cam1")
        self.assertEqual(goal.min_fps, 20.0)

    def test_english_no_fps(self):
        goal = parse_goal("reduce p95 latency, keep cam1 fps above some level")
        self.assertFalse(goal.unsupported)
        self.assertEqual(goal.min_fps, 15.0)

    def test_missing_camera_rejected(self):
        goal = parse_goal("降低全局 P95 延迟")
        self.assertTrue(goal.unsupported)
        self.assertIn("cam", goal.unsupported_reason)

    def test_missing_latency_intent_rejected(self):
        goal = parse_goal("保证 cam1 fps 不低于 15")
        self.assertTrue(goal.unsupported)
        self.assertIn("P95", goal.unsupported_reason)

    def test_custom_default(self):
        goal = parse_goal("保证 cam3 帧率达标并降低延迟", default_min_fps=10.0)
        self.assertFalse(goal.unsupported)
        self.assertEqual(goal.min_fps, 10.0)


if __name__ == "__main__":
    unittest.main()
