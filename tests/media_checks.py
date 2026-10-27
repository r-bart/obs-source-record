"""Assertions for synthetic files produced by the disposable OBS runner."""
import json
import subprocess
import time


def wait_for(predicate, timeout=20, message="condition"):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.2)
    raise AssertionError(f"Timeout: {message}")


def probe(path):
    result = subprocess.run(["ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", str(path)],
                            check=True, capture_output=True, text=True)
    return json.loads(result.stdout)


def decode(path):
    subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-i", str(path), "-f", "null", "-"],
                   check=True, capture_output=True, timeout=30)
    info = probe(path)
    assert any(s["codec_type"] == "video" for s in info["streams"]), path
    assert float(info["format"]["duration"]) > .3, path
    return info


def stable(paths):
    last, since = None, time.monotonic()
    def check():
        nonlocal last, since
        sizes = tuple(p.stat().st_size for p in paths)
        if sizes != last:
            last, since = sizes, time.monotonic()
        return time.monotonic()-since > 3
    wait_for(check, message="all stopped files finalized and stable")
