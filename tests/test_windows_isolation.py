import hashlib
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from isolated_obs import IsolationError
from isolated_obs_windows import verify_windows_instance


class WindowsIsolationTests(unittest.TestCase):
    def test_identity_portable_mode_plugin_and_listener_are_all_required(self):
        with tempfile.TemporaryDirectory(prefix="sr-windows-identity-") as directory:
            root = Path(directory).resolve()
            exe = root / "obs/bin/64bit/obs64.exe"
            plugin = root / "obs/obs-plugins/64bit/source-record.dll"
            for path in (exe, plugin):
                path.parent.mkdir(parents=True)
                path.write_bytes(b"synthetic fixture")
            (root / "obs/portable_mode.txt").touch()
            (root / "obs/config/obs-studio").mkdir(parents=True)
            manifest = root / "instance.json"
            manifest.write_text(json.dumps({"platform": "windows", "root": str(root), "pid": 123,
                "starttime": 42, "port": 12345, "plugin": str(plugin),
                "plugin_sha256": hashlib.sha256(plugin.read_bytes()).hexdigest(),
                "executable_sha256": hashlib.sha256(exe.read_bytes()).hexdigest()}))
            process = Mock()
            process.create_time.return_value = 42
            process.exe.return_value = str(exe)
            process.cwd.return_value = str(exe.parent)
            process.cmdline.return_value = [str(exe), "--portable"]
            process.memory_maps.return_value = [SimpleNamespace(path=str(plugin))]
            process.net_connections.return_value = [SimpleNamespace(status="LISTEN", laddr=SimpleNamespace(port=12345))]
            psutil = SimpleNamespace(Process=Mock(return_value=process), CONN_LISTEN="LISTEN", Error=OSError)
            with patch.dict("sys.modules", psutil=psutil):
                self.assertEqual(verify_windows_instance(manifest)["pid"], 123)
                for method, invalid in (("create_time", 43), ("exe", str(root / "personal-obs.exe")),
                        ("cwd", str(root)), ("cmdline", [str(exe)]),
                        ("memory_maps", [SimpleNamespace(path=str(root / "personal/source-record.dll"))]),
                        ("net_connections", [SimpleNamespace(status="LISTEN", laddr=SimpleNamespace(port=4455))])):
                    with self.subTest(method=method):
                        original = getattr(process, method).return_value
                        getattr(process, method).return_value = invalid
                        with self.assertRaises(IsolationError):
                            verify_windows_instance(manifest)
                        getattr(process, method).return_value = original
                plugin.write_bytes(b"replaced plugin")
                with self.assertRaises(IsolationError):
                    verify_windows_instance(manifest)


if __name__ == "__main__":
    unittest.main()
