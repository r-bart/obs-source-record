from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from isolated_obs import IsolationError, wait_until_ready


class RequestError(Exception):
    def __init__(self, code):
        self.code = code


class ReadinessTests(unittest.TestCase):
    def run_probe(self, responses, times=None):
        client = Mock()
        client.get_version.side_effect = responses
        context = Mock()
        context.__enter__ = Mock(return_value=client)
        context.__exit__ = Mock(return_value=False)
        constructor = Mock(return_value=context)
        modules = {"obsws_python": SimpleNamespace(ReqClient=constructor),
                   "obsws_python.error": SimpleNamespace(OBSSDKRequestError=RequestError)}
        with patch.dict("sys.modules", modules), patch("isolated_obs.time.sleep"), \
                patch("isolated_obs.time.monotonic", side_effect=times or [0, 1]):
            result = wait_until_ready({"port": 12345, "password": "synthetic"})
        constructor.assert_called_once_with(host="127.0.0.1", port=12345, password="synthetic", timeout=5)
        context.__exit__.assert_called_once()
        return result, client.get_version.call_count

    def test_retries_initialization_without_releasing_the_client_early(self):
        result, calls = self.run_probe([RequestError(207), "ready"])
        self.assertEqual((result, calls), ("ready", 2))

    def test_other_request_errors_are_not_retried(self):
        with self.assertRaises(RequestError) as raised:
            self.run_probe([RequestError(600)])
        self.assertEqual(raised.exception.code, 600)

    def test_initialization_wait_has_a_deadline(self):
        with self.assertRaises(IsolationError):
            self.run_probe([RequestError(207)], [0, 31])


if __name__ == "__main__":
    unittest.main()
