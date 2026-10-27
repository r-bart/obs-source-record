"""Own one disposable Linux OBS; never attach to an existing instance.

Run on a dedicated runner/VM: python tests/isolated_obs.py /path/to/obs TEST.py
The runner verifies socket ownership through procfs before allowing any client.
macOS/Windows are deliberately unsupported until their isolation is verified.
"""
import hashlib
import json
import shutil
import os
from pathlib import Path
import secrets
import signal
import socket
import subprocess
import sys
import tempfile
import time


PROC_ROOT = Path("/proc")


class IsolationError(RuntimeError):
    pass


def process_identity(pid):
    # comm may contain spaces or parentheses; starttime is field 22.
    return (PROC_ROOT / str(pid) / "stat").read_text().rsplit(")", 1)[1].split()[19]


def verify_instance(manifest=None):
    if not sys.platform.startswith("linux"):
        raise IsolationError("Integration requires the dedicated Linux runner; no local OBS connection allowed")
    path = Path(manifest or os.environ.get("SR_TEST_MANIFEST", ""))
    if not path.is_file():
        raise IsolationError("Missing runner manifest; attaching to OBS is disabled")
    data = json.loads(path.read_text())
    root = path.parent.resolve()
    if Path(data["root"]).resolve() != root or not (root / "config/obs-studio").is_dir():
        raise IsolationError("Invalid test configuration root")
    pid = int(data["pid"])
    if process_identity(pid) != data["starttime"]:
        raise IsolationError("Test process exited or PID was reused")
    env = (PROC_ROOT / str(pid) / "environ").read_bytes().split(b"\0")
    if f"XDG_CONFIG_HOME={root / 'config'}".encode() not in env:
        raise IsolationError("Process does not own the isolated configuration")
    plugin = Path(data["plugin"]).resolve()
    if not plugin.is_relative_to(root) or hashlib.sha256(plugin.read_bytes()).hexdigest() != data["plugin_sha256"]:
        raise IsolationError("Test plugin identity mismatch")
    if str(plugin) not in (PROC_ROOT / str(pid) / "maps").read_text():
        raise IsolationError("The expected test plugin is not loaded in this process")
    endpoint = f"0100007F:{int(data['port']):04X}"
    owned = set()
    for descriptor in (PROC_ROOT / str(pid) / "fd").iterdir():
        try:
            owned.add(descriptor.readlink().as_posix())
        except FileNotFoundError:
            pass
    for row in (PROC_ROOT / "net/tcp").read_text().splitlines()[1:]:
        fields = row.split()
        # OBS may bind all IPv4 interfaces. Authentication is mandatory.
        if fields[1] in (endpoint, f"00000000:{int(data['port']):04X}") and fields[3] == "0A":
            if f"socket:[{fields[9]}]" in owned:
                return data
    raise IsolationError("WebSocket listener is not owned by the test process")


def connect():
    data = verify_instance()
    import obsws_python as obs
    return obs.ReqClient(host="127.0.0.1", port=data["port"], password=data["password"], timeout=5)


def stop_owned(timeout=30):
    data = verify_instance()
    pid = int(data["pid"])
    os.kill(pid, signal.SIGINT)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            stat = (PROC_ROOT / str(pid) / "stat").read_text().rsplit(")", 1)[1].split()
            if stat[0] == "Z" or stat[19] != data["starttime"]:
                return True
        except FileNotFoundError:
            return True
        time.sleep(.1)
    return False


def main():
    if not sys.platform.startswith("linux"):
        raise IsolationError("Use a dedicated Linux runner/VM, not the user's OBS installation")
    if len(sys.argv) != 3:
        raise IsolationError("Usage: isolated_obs.py /absolute/path/to/obs test_script.py")
    binary, script = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    plugin_source = Path(os.environ.get("SR_TEST_PLUGIN", "")).resolve()
    if not plugin_source.is_file() or plugin_source.suffix != ".so":
        raise IsolationError("SR_TEST_PLUGIN must identify the explicitly built test plugin")
    root = Path(tempfile.mkdtemp(prefix="source-record-test-"))
    config = root / "config/obs-studio"
    ws = config / "plugin_config/obs-websocket"
    ws.mkdir(parents=True)
    plugin = config / "plugins/source-record/bin/64bit/source-record.so"
    plugin.parent.mkdir(parents=True)
    shutil.copy2(plugin_source, plugin)
    shutil.copytree(Path(__file__).resolve().parents[1] / "data", config / "plugins/source-record/data")
    profile = config / "basic/profiles/SR_TEST"
    profile.mkdir(parents=True)
    (profile / "basic.ini").write_text(
        "[General]\nName=SR_TEST\n[Output]\nMode=Simple\n"
        f"[SimpleOutput]\nFilePath={root}\nRecFormat=mp4\nRecFormat2=mp4\nRecEncoder=x264\n"
        "RecQuality=Small\nStreamEncoder=x264\nABitrate=128\nVBitrate=2500\n"
        "[Video]\nBaseCX=640\nBaseCY=360\nOutputCX=640\nOutputCY=360\nFPSType=0\nFPSCommon=30\n")
    user = "[General]\nFirstRun=true\n[Basic]\nProfile=SR_TEST\nProfileDir=SR_TEST\nSceneCollection=SR_TEST\nSceneCollectionFile=SR_TEST\n"
    (config / "global.ini").write_text(user)
    (config / "user.ini").write_text(user)
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    password = secrets.token_urlsafe(32)
    (ws / "config.json").write_text(json.dumps({"server_enabled": True, "server_port": port,
        "server_password": password, "auth_required": True, "alerts_enabled": False, "first_load": False}))
    env = dict(os.environ, XDG_CONFIG_HOME=str(root / "config"), XDG_DATA_HOME=str(root / "data"),
               XDG_CACHE_HOME=str(root / "cache"), TMPDIR=str(root), SR_TEST_MANIFEST=str(root / "instance.json"))
    for key in ("OBS_WS_HOST", "OBS_WS_PORT", "OBS_WS_PASSWORD", "SR_MANAGE_OBS"):
        env.pop(key, None)
    proc = None
    result = 2
    try:
        with (root / "obs.log").open("w") as log:
            proc = subprocess.Popen([str(binary), "--multi", "--disable-shutdown-check", "--websocket_ipv4_only", "--profile", "SR_TEST", "--collection", "SR_TEST"], env=env,
                                    stdout=log, stderr=subprocess.STDOUT)
            identity = process_identity(proc.pid)
            manifest = root / "instance.json"
            manifest.write_text(json.dumps({"root": str(root), "pid": proc.pid, "starttime": identity,
                                            "port": port, "password": password, "plugin": str(plugin),
                                            "plugin_sha256": hashlib.sha256(plugin.read_bytes()).hexdigest()}))
            manifest.chmod(0o600)
            deadline = time.monotonic() + 60
            while True:
                if proc.poll() is not None:
                    raise IsolationError(f"Test OBS exited with {proc.returncode}")
                try:
                    verify_instance(manifest)
                    break
                except IsolationError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(.2)
            result = subprocess.run([sys.executable, str(script)], env=env).returncode
    finally:
        if proc is not None and proc.poll() is None:
            # Popen owns this exact child; never target an application name.
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)
                print("FAIL: isolated OBS shutdown timed out", file=sys.stderr)
                result = 1
        if proc is not None and proc.returncode not in (None, 0):
            result = 1
        print(f"Test artefacts retained: {root}", file=sys.stderr)
    return result


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (IsolationError, OSError, ValueError, KeyError) as error:
        print(f"ABORT: {error}", file=sys.stderr)
        sys.exit(2)
