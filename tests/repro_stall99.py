#!/usr/bin/env python3
"""Regression against a runner-owned, disposable OBS only.

Launch through tests/isolated_obs.py on a dedicated Linux runner/VM.
Ambient OBS_WS_* credentials and attach-to-existing-OBS are not supported.
The current user's OBS must never be used as a target.
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

from isolated_obs import connect, stop_owned, IsolationError

logging.getLogger("obsws_python").setLevel(logging.CRITICAL)  # quiet caught request errors

VENDOR = "source-record"
SUF = str(os.getpid())
V_SCENE, V_SRC, V_FILT = f"V99_SC_{SUF}", f"V99_SRC_{SUF}", "V99_FILTER"
CHURNS = int(os.environ.get("SR_CHURNS", "3"))
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
    if stop_owned(SHUTDOWN_TIMEOUT):
        return "CLEAN", PASS
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

    tmp_v = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"sr99_v_{SUF}")
    tmp_b = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"sr99_b_{SUF}")
    os.makedirs(tmp_v, exist_ok=True)
    os.makedirs(tmp_b, exist_ok=True)

    proc = None
    cl = None
    code = ABORT
    try:
        cl = connect()
        proc = True  # the dedicated runner owns and authenticates the process
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
        # The runner retains synthetic media and logs for diagnosis.
    return code


if __name__ == "__main__":
    try:
        sys.exit(main())
    except IsolationError as error:
        print(f"ABORT: {error}", file=sys.stderr)
        sys.exit(2)
