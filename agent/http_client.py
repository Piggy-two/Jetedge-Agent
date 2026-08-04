"""Control API HTTP client with a requests→urllib fallback (Stage 12).

Only loopback (the server binds 127.0.0.1).  Unwraps the
{success, data, error_code, error} envelope; anything unexpected raises
ControlApiError.  Requests-optional: if `requests` is missing on the Jetson,
urllib is used transparently.
"""

from __future__ import annotations

import json
import ssl
import urllib.error
import urllib.request
from typing import Optional

from agent.schemas import (
    ENVELOPE_DATA,
    ENVELOPE_ERROR,
    ENVELOPE_ERROR_CODE,
    ENVELOPE_SUCCESS,
)

try:  # pragma: no cover - environment dependent
    import requests  # type: ignore

    _HAS_REQUESTS = True
except ImportError:  # pragma: no cover
    _HAS_REQUESTS = False


class ControlApiError(Exception):
    """Control API request failure (HTTP error or error envelope)."""

    def __init__(self, code: str, message: str, http_status: int = 0):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message
        self.http_status = http_status


class HttpTransport:
    """Thin facade: get(path) / post(path, body).  Mock-friendly."""

    def __init__(self, base_url: str, timeout_s: float = 10.0):
        self.base_url = base_url.rstrip("/")
        self.timeout_s = timeout_s
        self._impl = _RequestsImpl() if _HAS_REQUESTS else _UrllibImpl()

    def get(self, path: str, timeout_s: Optional[float] = None) -> dict:
        return self._request("GET", path, None, timeout_s)

    def post(self, path: str, body: dict, timeout_s: Optional[float] = None) -> dict:
        return self._request("POST", path, body, timeout_s)

    def _request(self, method: str, path: str, body: Optional[dict],
                 timeout_s: Optional[float]) -> dict:
        timeout = timeout_s if timeout_s is not None else self.timeout_s
        status, raw = self._impl.request(self.base_url, method, path, body, timeout)
        try:
            envelope = json.loads(raw)
        except (ValueError, TypeError) as exc:
            raise ControlApiError("HTTP_PARSE", f"non-JSON response ({exc})", status)
        if not isinstance(envelope, dict):
            raise ControlApiError("HTTP_PARSE", "response is not an object", status)
        if envelope.get(ENVELOPE_SUCCESS) is True:
            data = envelope.get(ENVELOPE_DATA)
            return data if isinstance(data, dict) else {}
        code = str(envelope.get(ENVELOPE_ERROR_CODE, "UNKNOWN"))
        message = str(envelope.get(ENVELOPE_ERROR, "unknown error"))
        raise ControlApiError(code, message, status)


class _RequestsImpl:  # pragma: no cover - depends on environment
    def request(self, base_url, method, path, body, timeout):
        url = base_url + path
        kwargs = {"timeout": timeout}
        if body is not None:
            kwargs["json"] = body
        resp = requests.request(method, url, **kwargs)
        return resp.status_code, resp.text


class _UrllibImpl:  # pragma: no cover - fallback path
    def request(self, base_url, method, path, body, timeout):
        url = base_url + path
        data = None
        headers = {}
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(url, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:  # nosec - loopback only
                return resp.status, resp.read().decode("utf-8", errors="replace")
        except urllib.error.HTTPError as exc:
            # Control API error envelopes arrive over 4xx/5xx; read the body.
            raw = exc.read().decode("utf-8", errors="replace")
            return exc.code, raw
        except (urllib.error.URLError, TimeoutError, ssl.SSLError, OSError) as exc:
            raise ControlApiError("NETWORK", str(exc)) from exc
