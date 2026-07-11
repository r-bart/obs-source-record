#!/usr/bin/env python3
"""Repro for issue #99: a Source Record recording stalls (file stops growing)
the moment a SECOND Source Record output starts/stops.

Replays the exact churn sequence observed in the 2026-07-11 stall logs:
victim filter records a color source; then a throwaway 2nd filter runs
replay_buffer_start -> stop -> record_start -> record_stop and is deleted.
After each churn round the victim's file growth is sampled; zero growth over a
trailing window while the output is still "active" = STALL reproduced.

Connection via env (OBS_WS_HOST/PORT/PASSWORD). Records into $TMPDIR, never
into the real recordings folder. Prints machine-greppable RESULT lines.
"""
import glob
import os
import struct
import sys
import time

import obsws_python as obs

HOST = os.environ.get("OBS_WS_HOST", "localhost")
PORT = int(os.environ.get("OBS_WS_PORT", "4455"))
PW = os.environ.get("OBS_WS_PASSWORD", "")
VENDOR = "source-record"
SUF = str(os.getpid())
V_SCENE, V_SRC, V_FILT = f"V99_SC_{SUF}", f"V99_SRC_{SUF}", "V99_FILTER"
CHURNS = int(os.environ.get("SR_CHURNS", "3"))


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
    cl.get_source_filter_list(src)
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
    cl.remove_input(src)
    cl.remove_scene(sc)


def cleanup(cl):
    for name in [V_SRC] + [f"B99_SRC_{SUF}_{i}" for i in range(CHURNS + 1)]:
        try:
            cl.remove_input(name)
        except Exception:
            pass
    for name in [V_SCENE] + [f"B99_SC_{SUF}_{i}" for i in range(CHURNS + 1)]:
        try:
            cl.remove_scene(name)
        except Exception:
            pass


def main():
    if not PW:
        print("OBS_WS_PASSWORD not set", file=sys.stderr)
        return 2
    tmp_v = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"sr99_v_{SUF}")
    tmp_b = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"sr99_b_{SUF}")
    os.makedirs(tmp_v, exist_ok=True)
    os.makedirs(tmp_b, exist_ok=True)

    cl = obs.ReqClient(host=HOST, port=PORT, password=PW, timeout=5)
    print("Connected:", cl.get_version().obs_version, flush=True)

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
        print("RESULT: victim never created a file — abort")
        cleanup(cl)
        return 2

    base = growth(tmp_v, 8)
    rate = sum(base) / 8.0
    print(f"BASELINE_RATE={rate/1024:.0f}KB/s samples={[d//1024 for d in base]}", flush=True)
    if rate <= 0:
        print("RESULT: victim not growing at baseline — abort")
        cleanup(cl)
        return 2

    stalled_at = None
    for i in range(1, CHURNS + 1):
        print(f"--- churn {i} ---", flush=True)
        churn(cl, i, tmp_b)
        deltas = growth(tmp_v, 30)
        print(f"CHURN_{i}_GROWTH_KB={[d//1024 for d in deltas]}", flush=True)
        tail = deltas[-6:]  # trailing 12s
        if sum(tail) == 0:
            stalled_at = i
            break

    alive = cl.get_version().obs_version
    print(f"OBS_ALIVE={bool(alive)}", flush=True)

    vfile = newest(tmp_v)
    if stalled_at:
        print(f"VERDICT=STALL churn={stalled_at} file={vfile} size={fsize(tmp_v)}", flush=True)
        # try to stop the stalled recording — does it finalize?
        vendor(cl, "record_stop", {"source": V_SRC})
        time.sleep(5)
        post = growth(tmp_v, 6)
        print(f"POST_STOP_GROWTH_KB={[d//1024 for d in post]}", flush=True)
        print(f"ATOMS={atoms(vfile)}", flush=True)
        print(f"FINALIZED={'moov' in atoms(vfile)}", flush=True)
    else:
        vendor(cl, "record_stop", {"source": V_SRC})
        time.sleep(3)
        print(f"VERDICT=HEALTHY churns={CHURNS} file={vfile}", flush=True)
        if vfile:
            print(f"ATOMS={atoms(vfile)}", flush=True)
            print(f"FINALIZED={'moov' in atoms(vfile)}", flush=True)

    cleanup(cl)
    return 0 if stalled_at else 1


if __name__ == "__main__":
    sys.exit(main())
