"""DeepSeek client + circuit breaker + key loading unit tests (Stage 12).

Transport is mocked — nothing touches the network.
"""

from __future__ import annotations

import json
import os
import tempfile
import unittest
import urllib.error
from unittest import mock

from agent.deepseek_client import (
    CircuitBreaker,
    DeepSeekClient,
    DeepSeekError,
    _parse_arguments,
    load_api_key,
    read_secrets_file,
)


class FakeClock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t

    def advance(self, dt):
        self.t += dt


class CircuitBreakerTest(unittest.TestCase):
    def test_closed_to_open_to_half_open_to_closed(self):
        clock = FakeClock()
        cb = CircuitBreaker(fail_threshold=3, reset_s=10.0,
                            half_open_success=2, clock=clock)
        for _ in range(3):
            cb.record_failure()
        self.assertEqual(cb.state_name(), "OPEN")
        self.assertFalse(cb.allow())          # still within reset window
        clock.advance(10.0)
        self.assertTrue(cb.allow())           # -> HALF_OPEN
        cb.record_success()
        cb.record_success()
        self.assertEqual(cb.state_name(), "CLOSED")
        self.assertTrue(cb.allow())

    def test_half_open_failure_reopens(self):
        clock = FakeClock()
        cb = CircuitBreaker(fail_threshold=1, reset_s=10.0, clock=clock)
        cb.record_failure()
        clock.advance(10.0)
        self.assertTrue(cb.allow())
        cb.record_failure()
        self.assertEqual(cb.state_name(), "OPEN")


class ParseArgumentsTest(unittest.TestCase):
    def test_plain_json(self):
        self.assertEqual(_parse_arguments('{"stream_id": "cam4", "interval": 1}'),
                         {"stream_id": "cam4", "interval": 1})

    def test_markdown_fence(self):
        raw = '```json\n{"stream_id": "cam4", "interval": 1}\n```'
        self.assertEqual(_parse_arguments(raw),
                         {"stream_id": "cam4", "interval": 1})

    def test_invalid_json_empty(self):
        self.assertEqual(_parse_arguments("not json"), {})
        self.assertEqual(_parse_arguments('"a string"'), {})
        self.assertEqual(_parse_arguments(""), {})


class DeepSeekClientTest(unittest.TestCase):
    def _client(self, clock=None, fail_threshold=2):
        cfg = {"deepseek_endpoint": "https://example.invalid/v1/chat/completions",
               "deepseek_model": "deepseek-v4-flash",
               "deepseek_timeout_s": 5.0, "deepseek_max_retries": 1,
               "deepseek_max_tokens": 64,
               "breaker_fail_threshold": fail_threshold, "breaker_reset_s": 30.0,
               "breaker_half_open_success": 2}
        return DeepSeekClient(cfg, api_key="test-key", clock=clock or FakeClock())

    def _tool_call_response(self, name="set_infer_interval", arguments=None):
        return {"choices": [{"message": {"role": "assistant", "content": None,
                                         "tool_calls": [{"function": {
                                             "name": name,
                                             "arguments": json.dumps(arguments or {})}}]}}]}

    def test_parses_tool_calls(self):
        client = self._client()
        with mock.patch("urllib.request.urlopen") as urlopen:
            cm = mock.MagicMock()
            cm.read.return_value = json.dumps(
                self._tool_call_response(arguments={"stream_id": "cam4",
                                                    "interval": 1})).encode()
            urlopen.return_value.__enter__.return_value = cm
            calls = client.plan_candidates("sys", "obs", [])
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0].name, "set_infer_interval")
        self.assertEqual(calls[0].args, {"stream_id": "cam4", "interval": 1})
        self.assertEqual(client.breaker.state_name(), "CLOSED")

    def test_empty_tool_calls(self):
        client = self._client()
        with mock.patch("urllib.request.urlopen") as urlopen:
            cm = mock.MagicMock()
            cm.read.return_value = json.dumps(
                {"choices": [{"message": {"role": "assistant", "content": "no"}}]}).encode()
            urlopen.return_value.__enter__.return_value = cm
            calls = client.plan_candidates("sys", "obs", [])
        self.assertEqual(calls, [])

    def test_http_error_retries_then_breaks_circuit(self):
        client = self._client(fail_threshold=1)  # one failed call opens it
        with mock.patch("urllib.request.urlopen",
                        side_effect=TimeoutError("t")) as urlopen:
            # 1 initial + 1 retry (max_retries=1), then raise.
            with self.assertRaises(DeepSeekError):
                client.plan_candidates("sys", "obs", [])
        self.assertEqual(urlopen.call_count, 2)  # retried once internally
        self.assertEqual(client.breaker.state_name(), "OPEN")
        with self.assertRaises(DeepSeekError):
            client.plan_candidates("sys", "obs", [])  # circuit open, no call
        self.assertEqual(urlopen.call_count, 2)

    def test_client_error_no_retry(self):
        client = self._client()
        err = urllib.error.HTTPError("https://example.invalid", 400, "bad", {}, None)
        with mock.patch("urllib.request.urlopen", side_effect=err) as urlopen:
            with self.assertRaises(DeepSeekError):
                client.plan_candidates("sys", "obs", [])
        self.assertEqual(urlopen.call_count, 1)


class SecretsTest(unittest.TestCase):
    def test_read_secrets_file(self):
        with tempfile.NamedTemporaryFile("w", suffix=".env", delete=False) as fh:
            fh.write("# comment\n\nDEEPSEEK_API_KEY=sk-secret-123\nOTHER=1\n")
            path = fh.name
        try:
            secrets = read_secrets_file(path)
            self.assertEqual(secrets.get("DEEPSEEK_API_KEY"), "sk-secret-123")
            self.assertEqual(secrets.get("OTHER"), "1")
        finally:
            os.unlink(path)

    def test_env_var_takes_precedence(self):
        with mock.patch.dict(os.environ, {"JETEDGE_DEEPSEEK_API_KEY": "env-key"}):
            key = load_api_key({"secrets_file": "/nonexistent"})
        self.assertEqual(key, "env-key")

    def test_missing_everywhere_empty(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            key = load_api_key({"secrets_file": "/nonexistent"})
        self.assertEqual(key, "")


if __name__ == "__main__":
    unittest.main()
