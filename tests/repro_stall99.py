#!/usr/bin/env python3
"""Regression test for issue #99: a Source Record recording must NOT stall when a
second Source Record output starts/stops, and OBS must shut down cleanly.

Replays the churn sequence from the 2026-07-11 stall logs: a victim filter
records a color source, then a throwaway 2nd filter runs
replay_buffer_start -> stop -> record_start -> record_stop and is deleted. The
victim must keep growing across every churn and finalize a playable mp4 (moov).

Two failure arms of #99 are checked:
  * graphics-thread freeze  -> victim file stops growing (asserted always);
  * destruction-thread hang -> OBS shutdown never completes. This only surfaces
    at shutdown, so it is checked in --manage-obs mode, where this script owns
    the OBS process and asserts it exits within a timeout. Over an attached
    (externally launched) OBS the shutdown arm cannot be observed and is
    reported as SKIPPED rather than silently passed.

Exit codes (0 = pass, standard CI semantics):
  0  PASS   — healthy across churns, moov finalized, clean shutdown (or shutdown SKIPPED in attach mode)
  1  FAIL   — stall reproduced (victim stopped growing)
  2  ABORT  — setup/precondition failure
  3  FAIL   — victim never finalized (no moov)
  4  FAIL   — OBS shutdown hung (destruction-thread arm)

Connection via env: OBS_WS_HOST (localhost), OBS_WS_PORT (4455), OBS_WS_PASSWORD.
Managed mode: SR_MANAGE_OBS=1 (or --manage-obs); OBS_APP_PATH overrides the app
bundle (default /Applications/OBS.app). Records into $TMPDIR (cleaned up on exit).
"""
import glob
import logging
import os
import shutil
import signal
import struct
import subprocess
import sys
import time

import obsws_python as obs

logging.getLogger("obsws_python").setLevel(logging.CRITICAL)  # quiet caught request errors

HOST = os.environ.get("OBS_WS_HOST", "localhost")
PORT = int(os.environ.get("OBS_WS_PORT", "4455"))
PW = os.environ.get("OBS_WS_PASSWORD", "")
VENDOR = "source-record"
SUF = str(os.getpid())
V_SCENE, V_SRC, V_FILT = f"V99_SC_{SUF}", f"V99_SRC_{SUF}", "V99_FILTER"
CHURNS = int(os.environ.get("SR_CHURNS", "3"))
MANAGE_OBS = os.environ.get("SR_MANAGE_OBS") == "1" or "--manage-obs" in sys.argv
OBS_APP = os.environ.get("OBS_APP_PATH", "/Applications/OBS.app")
SHUTDOWN_TIMEOUT = int(os.environ.get("SR_SHUTDOWN_TIMEOUT", "30"))

# Exit codes
PASS, FAIL_STALL, ABORT, FAIL_NO_MOOV, FAIL_SHUTDOWN_HANG = 0, 1, 2, 3, 4


def vendor(cl, rt, data=None):
    r = cl.send("CallVendorRequest", {"vendorName": VENDOR, "requestType": rt, "requestData": data or {}}, raw=True)
    return r.get("responseData", {})


def newest(d):
    files = glob.glob(os.path.join(d, "*"))
    return max(files, key=os.path.getmtime) if files else None


def fsize(d):
    f = newest(d)
    return os.path.getsize(f) if f else 0


def atoms(path):
    """Top-level mp4 atom names."""
    out, off, end = [], 0, os.path.getsize(path)
    with open(path, "rb") as fh:
        while off + 8 <= end:
            fh.seek(off)
            hdr = fh.read(16)
            size = struct.unpack(">I", hdr[:4])[0]
            name = hdr[4:8].decode("latin1")
            if size == 1:
                size = struct.unpack(">Q", hdr[8:16])[0]
            elif size == 0:
                size = end - off
            out.append(name)
            if size < 8:
                break
            off += size
    return out


def growth(d, seconds, step=2.0):
    """Sample newest-file size; return list of deltas per step."""
    deltas, prev = [], fsize(d)
    t_end = time.time() + seconds
    while time.time() < t_end:
        time.sleep(step)
        cur = fsize(d)
        deltas.append(cur - prev)
        prev = cur
    return deltas


def wait_ws(timeout):
    """Wait for the websocket to accept a connection; return a client or None."""
    import socket
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = socket.socket()
        s.settimeout(1)
        try:
            s.connect((HOST, PORT))
            s.close()
            return obs.ReqClient(host=HOST, port=PORT, password=PW, timeout=5)
        except Exception:
            time.sleep(1)
        finally:
            s.close()
    return None


def launch_obs():
    """Launch OBS directly so we own the PID; return the Popen handle."""
    binary = os.path.join(OBS_APP, "Contents/MacOS/OBS")
    if not os.path.exists(binary):
        return None
    # cwd must be the MacOS dir for OBS to find its data relative paths
    return subprocess.Popen([binary, "--disable-shutdown-check"], cwd=os.path.dirname(binary))


def churn(cl, i, tmp_b):
    """One round of the killer sequence from the stall logs."""
    sc, src, filt = f"B99_SC_{SUF}_{i}", f"B99_SRC_{SUF}_{i}", f"B99_FILTER_{i}"
    cl.create_scene(sc)
    kinds = cl.get_input_kind_list(False).input_kinds
    ck = next((k for k in kinds if k.startswith("color_source")), "color_source_v3")
    cl.create_input(sc, src, ck, {"width": 320, "height": 240}, True)
    cl.create_source_filter(src, filt, "source_record_filter",
                            {"path": tmp_b, "record_mode": 0, "stream_mode": 0})
    vendor(cl, "replay_buffer_start", {"source": src, "filename": "rb_%CCYY"})
    time.sleep(0.3)
    vendor(cl, "replay_buffer_stop", {"source": src})
    time.sleep(0.2)
    vendor(cl, "record_pause", {"source": src})
    vendor(cl, "record_start", {"source": src})
    time.sleep(0.3)
    try:
        cl.set_source_filter_enabled(src, filt, False)
    except Exception:
        pass
    vendor(cl, "record_stop", {"source": src})
    time.sleep(0.3)
    for fn in (lambda: cl.remove_input(src), lambda: cl.remove_scene(sc)):
        try:
            fn()
        except Exception:
            pass


def cleanup(cl):
    # churn() already removes its own B99 artifacts each round; only the victim
    # scene/source and any B99 leftovers from a churn that aborted mid-round
    # remain. Remove the victim explicitly and sweep any surviving B99 names.
    survivors = [V_SRC] + [f"B99_SRC_{SUF}_{i}" for i in range(CHURNS + 1)]
    scenes = [V_SCENE] + [f"B99_SC_{SUF}_{i}" for i in range(CHURNS + 1)]
    existing = {i["inputName"] for i in cl.get_input_list().inputs}
    for name in survivors:
        if name in existing:
            try:
                cl.remove_input(name)
            except Exception:
                pass
    existing_scenes = {s["sceneName"] for s in cl.get_scene_list().scenes}
    for name in scenes:
        if name in existing_scenes:
            try:
                cl.remove_scene(name)
            except Exception:
                pass


def assert_clean_shutdown(cl, proc):
    """Managed mode: quit OBS and assert it exits within SHUTDOWN_TIMEOUT.
    Returns (verdict_str, exit_code)."""
    # graceful quit like a user would; falls back to SIGTERM
    subprocess.run(["osascript", "-e", 'tell application "OBS" to quit'],
                   capture_output=True, timeout=10)
    try:
        proc.wait(timeout=SHUTDOWN_TIMEOUT)
        print(f"SHUTDOWN=CLEAN in <= {SHUTDOWN_TIMEOUT}s (rc={proc.returncode})", flush=True)
        return "CLEAN", PASS
    except subprocess.TimeoutExpired:
        print(f"SHUTDOWN=HANG (OBS still alive after {SHUTDOWN_TIMEOUT}s) — destruction-thread arm", flush=True)
        try:
            proc.send_signal(signal.SIGKILL)
            proc.wait(timeout=10)
        except Exception:
            subprocess.run(["killall", "-9", "OBS"], capture_output=True)
        return "HANG", FAIL_SHUTDOWN_HANG


def run(cl, tmp_v, tmp_b, proc):
    """Core scenario. Returns exit code."""
    cl.create_scene(V_SCENE)
    kinds = cl.get_input_kind_list(False).input_kinds
    ck = next((k for k in kinds if k.startswith("color_source")), "color_source_v3")
    cl.create_input(V_SCENE, V_SRC, ck, {"width": 1920, "height": 1080}, True)
    cl.create_source_filter(V_SRC, V_FILT, "source_record_filter",
                            {"path": tmp_v, "record_mode": 0, "stream_mode": 0})
    time.sleep(1.0)
    vendor(cl, "record_start", {"source": V_SRC})
    time.sleep(2.0)

    if not newest(tmp_v):
        print("RESULT: victim never created a file — abort", flush=True)
        return ABORT

    base = growth(tmp_v, 8)
    rate = sum(base) / 8.0
    print(f"BASELINE_RATE={rate/1024:.0f}KB/s samples={[d//1024 for d in base]}", flush=True)
    if rate <= 0:
        print("RESULT: victim not growing at baseline — abort", flush=True)
        return ABORT

    stalled_at = None
    for i in range(1, CHURNS + 1):
        print(f"--- churn {i} ---", flush=True)
        churn(cl, i, tmp_b)
        deltas = growth(tmp_v, 30)
        print(f"CHURN_{i}_GROWTH_KB={[d//1024 for d in deltas]}", flush=True)
        if sum(deltas[-6:]) == 0:  # trailing 12s of zero growth = stalled
            stalled_at = i
            break

    # liveness ping guarded: a true hang must not raise a bare traceback
    try:
        alive = bool(cl.get_version().obs_version)
    except Exception as e:
        print(f"OBS_ALIVE=False ({type(e).__name__}) — websocket unresponsive", flush=True)
        alive = False
    print(f"OBS_ALIVE={alive}", flush=True)

    vfile = newest(tmp_v)
    vendor(cl, "record_stop", {"source": V_SRC})
    time.sleep(5)

    if stalled_at:
        print(f"VERDICT=STALL churn={stalled_at} file={vfile} size={fsize(tmp_v)}", flush=True)
        return FAIL_STALL

    finalized = bool(vfile) and "moov" in atoms(vfile)
    print(f"VERDICT=HEALTHY churns={CHURNS} file={vfile}", flush=True)
    print(f"ATOMS={atoms(vfile) if vfile else None}", flush=True)
    print(f"FINALIZED={finalized}", flush=True)
    if not finalized:
        print("RESULT: victim recorded but never finalized (no moov)", flush=True)
        return FAIL_NO_MOOV

    if proc is not None:
        verdict, code = assert_clean_shutdown(cl, proc)
        return code
    print("SHUTDOWN=SKIPPED (attach mode; run with --manage-obs to cover the shutdown-hang arm)", flush=True)
    return PASS


def main():
    if not PW:
        print("OBS_WS_PASSWORD not set", file=sys.stderr)
        return ABORT

    tmp_v = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"sr99_v_{SUF}")
    tmp_b = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"sr99_b_{SUF}")
    os.makedirs(tmp_v, exist_ok=True)
    os.makedirs(tmp_b, exist_ok=True)

    proc = None
    cl = None
    code = ABORT
    try:
        if MANAGE_OBS:
            proc = launch_obs()
            if proc is None:
                print(f"RESULT: OBS binary not found under {OBS_APP} — abort", flush=True)
                return ABORT
            print(f"Launched OBS pid={proc.pid}; waiting for websocket...", flush=True)
            cl = wait_ws(90)
        else:
            cl = wait_ws(10)
        if cl is None:
            print("RESULT: websocket never came up — abort", flush=True)
            return ABORT
        print("Connected:", cl.get_version().obs_version, flush=True)
        code = run(cl, tmp_v, tmp_b, proc)
    finally:
        if cl is not None:
            try:
                cleanup(cl)
            except Exception:
                pass
        # if we own OBS and did not already quit it (non-managed run path), leave it;
        # managed run quits inside assert_clean_shutdown. Kill a still-alive managed proc.
        if proc is not None and proc.poll() is None:
            try:
                proc.send_signal(signal.SIGKILL)
            except Exception:
                pass
        shutil.rmtree(tmp_v, ignore_errors=True)
        shutil.rmtree(tmp_b, ignore_errors=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
