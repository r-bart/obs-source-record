"""Run one copied, portable OBS on Windows; never attach to the user's OBS.

Requires psutil and obsws-python. All binaries, configuration and outputs live
under a fresh directory in --work-root. The source installation is read only.
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def verify_windows_instance(manifest):
    from isolated_obs import IsolationError
    import psutil

    path = Path(manifest).resolve()
    data = json.loads(path.read_text())
    root = path.parent
    if data.get("platform") != "windows" or Path(data["root"]).resolve() != root:
        raise IsolationError("Invalid Windows runner manifest")
    executable = root / "obs/bin/64bit/obs64.exe"
    plugin = root / "obs/obs-plugins/64bit/source-record.dll"
    if not (root / "obs/portable_mode.txt").is_file() or not (root / "obs/config/obs-studio").is_dir():
        raise IsolationError("Missing private portable configuration")
    if Path(data["plugin"]).resolve() != plugin or digest(plugin) != data["plugin_sha256"]:
        raise IsolationError("Test plugin identity mismatch")
    try:
        process = psutil.Process(int(data["pid"]))
        if process.create_time() != data["starttime"] or Path(process.exe()).resolve() != executable:
            raise IsolationError("Process identity mismatch")
        if Path(process.cwd()).resolve() != executable.parent or "--portable" not in process.cmdline():
            raise IsolationError("Process is not running from its private portable directory")
        if digest(executable) != data["executable_sha256"]:
            raise IsolationError("Test executable identity mismatch")
        loaded = {Path(m.path).resolve() for m in process.memory_maps() if m.path.lower().endswith("source-record.dll")}
        if loaded != {plugin}:
            raise IsolationError("Expected private plugin is not the only loaded Source Record module")
        if not any(c.status == psutil.CONN_LISTEN and c.laddr.port == data["port"]
                   for c in process.net_connections(kind="tcp")):
            raise IsolationError("WebSocket listener is not owned by the test process")
    except (psutil.Error, OSError) as error:
        raise IsolationError(f"Cannot verify private process: {error}") from error
    return data


def close_windows_instance(data, timeout=30):
    """Send WM_CLOSE only to windows belonging to the verified PID."""
    import psutil
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    process = psutil.Process(data["pid"])
    if process.create_time() != data["starttime"]:
        raise RuntimeError("PID reused before close")
    @callback_type
    def close_window(hwnd, unused):
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value == data["pid"]:
            title = ctypes.create_unicode_buffer(512)
            user32.GetWindowTextW(hwnd, title, len(title))
            if title.value.startswith("OBS ") and "SR_TEST" in title.value:
                user32.PostMessageW(hwnd, 0x0010, 0, 0)
        return True
    user32.EnumWindows(close_window, 0)
    try:
        process.wait(timeout)
        return True
    except psutil.TimeoutExpired:
        return False


def snapshot(paths):
    result = {}
    for base in paths:
        base = Path(base)
        if base.is_file():
            result[str(base)] = digest(base)
        elif base.is_dir():
            for path in base.rglob("*"):
                if path.is_file():
                    result[str(path)] = digest(path)
    return result


def main():
    if sys.platform != "win32":
        raise RuntimeError("Windows runner requires Windows")
    import psutil
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--obs-install", type=Path, required=True)
    parser.add_argument("--plugin", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("script", type=Path)
    args = parser.parse_args()
    protected = [args.obs_install.resolve(), Path(os.environ["APPDATA"]) / "obs-studio",
                 Path(os.environ["PROGRAMDATA"]) / "obs-studio/plugins"]
    args.work_root.mkdir(parents=True, exist_ok=True)
    if any(args.work_root.resolve().is_relative_to(p) for p in protected):
        raise RuntimeError("Work root must be outside the user's installation/configuration")
    before = snapshot(protected)
    root = Path(tempfile.mkdtemp(prefix="obs33-", dir=args.work_root.resolve()))
    obs = root / "obs"
    for name in ("bin", "core", "data", "obs-plugins"):
        shutil.copytree(args.obs_install / name, obs / name)
    (obs / "portable_mode.txt").touch()
    plugin = obs / "obs-plugins/64bit/source-record.dll"
    shutil.copy2(args.plugin, plugin)
    shutil.copytree(Path(__file__).resolve().parents[1] / "data", obs / "data/obs-plugins/source-record", dirs_exist_ok=True)
    config = obs / "config/obs-studio"
    profile = config / "basic/profiles/SR_TEST"
    profile.mkdir(parents=True)
    (config / "basic/scenes").mkdir(parents=True)
    # Disable automatic device discovery: the fixture uses synthetic media only.
    (profile / "basic.ini").write_text(
        "[General]\nName=SR_TEST\n[Output]\nMode=Simple\n"
        f"[SimpleOutput]\nFilePath={root.as_posix()}\nRecFormat=mp4\nRecFormat2=mp4\nRecEncoder=x264\n"
        "RecQuality=Small\nStreamEncoder=x264\nABitrate=128\nVBitrate=2500\n"
        "[Video]\nBaseCX=640\nBaseCY=360\nOutputCX=640\nOutputCY=360\nFPSType=0\nFPSCommon=30\n"
        "[Audio]\nSampleRate=48000\nChannelSetup=Stereo\n")
    (config / "basic/scenes/SR_TEST.json").write_text(json.dumps({
        "name": "SR_TEST", "current_scene": "Empty", "current_program_scene": "Empty",
        "scene_order": [{"name": "Empty"}], "sources": [{"name": "Empty", "id": "scene", "settings": {"items": []}}]}))
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    password = secrets.token_urlsafe(32)
    user = ("[General]\nFirstRun=true\nLastUpdateCheck=0\nEnableAutoUpdates=false\nConfirmOnExit=false\n"
            "[Basic]\nProfile=SR_TEST\nProfileDir=SR_TEST\nSceneCollection=SR_TEST\nSceneCollectionFile=SR_TEST\n"
            "[BasicWindow]\nSysTrayEnabled=false\nWarnBeforeStartingStream=false\nWarnBeforeStoppingStream=false\n"
            f"[OBSWebSocket]\nFirstLoad=false\nServerEnabled=true\nServerPort={port}\nAuthRequired=true\n"
            f"ServerPassword={password}\nAlertsEnabled=false\n")
    for name in ("global.ini", "user.ini"):
        (config / name).write_text(user)
    ws = config / "plugin_config/obs-websocket"
    ws.mkdir(parents=True)
    (ws / "config.json").write_text(json.dumps({"server_enabled": True, "server_port": port,
        "server_password": password, "auth_required": True, "alerts_enabled": False, "first_load": False}))
    env = dict(os.environ, SR_TEST_MANIFEST=str(root / "instance.json"), TMPDIR=str(root), TEMP=str(root), TMP=str(root))
    for key in list(env):
        if key.startswith("OBS_WS_") or key in ("OBS_PLUGINS_PATH", "OBS_PLUGINS_DATA_PATH", "OBS_USER_PLUGINS_PATH"):
            del env[key]
    # Libraries that consult the user environment also receive private directories.
    for key in ("APPDATA", "LOCALAPPDATA", "PROGRAMDATA"):
        target = root / key.lower()
        target.mkdir()
        env[key] = str(target)
    binary = obs / "bin/64bit/obs64.exe"
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    result, proc, data = 2, None, None
    started = time.monotonic()
    try:
        with (root / "obs.log").open("w") as log:
            proc = subprocess.Popen([str(binary), "--portable", "--multi", "--disable-shutdown-check",
                "--disable-updater", "--websocket_ipv4_only", "--profile", "SR_TEST", "--collection", "SR_TEST"],
                cwd=binary.parent, env=env, startupinfo=startup, stdout=log, stderr=subprocess.STDOUT)
            data = {"platform": "windows", "root": str(root), "pid": proc.pid,
                    "starttime": psutil.Process(proc.pid).create_time(), "port": port, "password": password,
                    "plugin": str(plugin), "plugin_sha256": digest(plugin), "executable_sha256": digest(binary)}
            (root / "instance.json").write_text(json.dumps(data))
            deadline = time.monotonic() + 60
            from isolated_obs import IsolationError
            while True:
                if proc.poll() is not None:
                    raise RuntimeError(f"Private OBS exited during startup: {proc.returncode}")
                try:
                    verify_windows_instance(root / "instance.json")
                    break
                except IsolationError:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(.5)
            print(f"Verified isolated OBS 33, PID {proc.pid}, plugin {data['plugin_sha256']}, root {root}", flush=True)
            logs = sorted((config / "logs").glob("*.txt"))
            if not logs:
                raise RuntimeError("Private OBS did not create its portable log")
            data["log"] = str(logs[-1])
            (root / "instance.json").write_text(json.dumps(data))
            result = subprocess.run([sys.executable, str(args.script.resolve())], env=env, timeout=600).returncode
    finally:
        if proc is not None and proc.poll() is None:
            if not close_windows_instance(data):
                print("FAIL: isolated Windows OBS shutdown timed out", file=sys.stderr)
                result = 1
                proc.kill()  # Exact Popen child; never an application-name kill.
            proc.wait(timeout=10)
        if proc is not None and proc.returncode != 0:
            result = 1
        validation = root / "validate-after-close.json"
        if result == 0 and validation.is_file():
            from media_checks import decode
            for expected in json.loads(validation.read_text())["files"]:
                path = Path(expected["path"]).resolve()
                if not path.is_relative_to(root):
                    raise RuntimeError("Post-shutdown validation path escaped the test root")
                info = decode(path)
                codec = next(s for s in info["streams"] if s["codec_type"] == "video")["codec_name"]
                if codec != expected["codec"]:
                    raise RuntimeError(f"Unexpected recorded codec: {codec}")
                print(f"PASS normal shutdown finalized and decoded {path.name}", flush=True)
        after = snapshot(protected)
        unchanged = before == after
        (root / "result.json").write_text(json.dumps({"result": result, "protected_unchanged": unchanged,
            "seconds": time.monotonic()-started, "plugin_sha256": data["plugin_sha256"] if data else None}, indent=2))
        if not unchanged:
            raise RuntimeError("Protected installation/configuration changed during the test")
        print(f"Protected installation and personal configuration hashes unchanged; artefacts: {root}", flush=True)
    return result


if __name__ == "__main__":
    sys.exit(main())
