import hashlib
import json
import os
import tempfile
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

import isolated_obs


class IsolationTests(unittest.TestCase):
    def test_unsupported_host_never_connects(self):
        with patch.object(isolated_obs.sys, "platform", "darwin"):
            with self.assertRaises(isolated_obs.IsolationError):
                isolated_obs.connect()

    def test_missing_manifest_never_uses_ambient_obs_credentials(self):
        with patch.object(isolated_obs.sys, "platform", "linux"), patch.dict(os.environ,
                {"SR_TEST_MANIFEST": "/nonexistent/sr-instance.json", "OBS_WS_HOST": "localhost",
                 "OBS_WS_PORT": "4455", "OBS_WS_PASSWORD": "personal"}):
            with self.assertRaises(isolated_obs.IsolationError):
                isolated_obs.connect()

    def test_owned_endpoint_and_reused_pid(self):
        with tempfile.TemporaryDirectory(prefix="sr-isolation-") as directory:
            root = Path(directory).resolve()
            (root / "config/obs-studio").mkdir(parents=True)
            plugin = root / "source-record.so"
            plugin.write_bytes(b"test binary")
            proc = root / "proc/123"
            (proc / "fd").mkdir(parents=True)
            (proc / "stat").write_text("123 (OBS with spaces) S " + "0 " * 18 + "42 0")
            (proc / "environ").write_bytes(f"XDG_CONFIG_HOME={root / 'config'}\0".encode())
            (proc / "maps").write_text(str(plugin))
            (proc / "fd/7").symlink_to("socket:[789]")
            (root / "proc/net").mkdir()
            listener = "0: 0100007F:1234 00000000:0000 0A 0 0 0 0 0 789"
            (root / "proc/net/tcp").write_text("header\n" + listener + "\n")
            manifest = root / "instance.json"
            manifest.write_text(json.dumps({"root": str(root), "pid": 123, "starttime": "42", "port": 0x1234,
                "plugin": str(plugin), "plugin_sha256": hashlib.sha256(plugin.read_bytes()).hexdigest(), "password": "test"}))
            with patch.object(isolated_obs.sys, "platform", "linux"), patch.object(isolated_obs, "PROC_ROOT", root / "proc"):
                self.assertEqual(isolated_obs.verify_instance(manifest)["pid"], 123)
                (proc / "fd/7").unlink()
                with self.assertRaises(isolated_obs.IsolationError):
                    isolated_obs.verify_instance(manifest)
                (proc / "fd/7").symlink_to("socket:[789]")
                (proc / "stat").write_text("123 (replacement) S " + "0 " * 18 + "43 0")
                with patch.dict(os.environ, {"SR_TEST_MANIFEST": str(manifest)}), patch.object(isolated_obs.os, "kill") as kill:
                    with self.assertRaises(isolated_obs.IsolationError):
                        isolated_obs.stop_owned()
                    kill.assert_not_called()

    def test_scripts_refuse_attach_without_runner(self):
        env = dict(os.environ, SR_TEST_MANIFEST="/nonexistent/sr-instance.json")
        for name in ("smoke_websocket.py", "repro_stall99.py", "repro_settings_restart.py", "repro_video_reset.py"):
            with self.subTest(script=name):
                result = subprocess.run([sys.executable, str(Path(__file__).parent/name)], env=env,
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 2, result.stdout+result.stderr)
                self.assertIn("ABORT:", result.stderr)


if __name__ == "__main__":
    unittest.main()
