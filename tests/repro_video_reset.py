#!/usr/bin/env python3
"""Regression against a runner-owned, disposable OBS only.

Launch through tests/isolated_obs.py on a dedicated Linux runner/VM.
Ambient OBS_WS_* credentials and attach-to-existing-OBS are not supported.
The current user's OBS must never be used as a target.
"""
import logging
import os
import sys
import time

from isolated_obs import connect, stop_owned, IsolationError

logging.getLogger("obsws_python").setLevel(logging.CRITICAL)
SUF = str(os.getpid())
SCENE, SRC = "SR_REPRO_" + SUF, "SR_REPRO_SRC_" + SUF
V = "source-record"


def vendor(cl, rt, data):
    r = cl.send("CallVendorRequest", {"vendorName": V, "requestType": rt, "requestData": data}, raw=True)
    return r.get("responseData", {})


def main():
    cl = connect()
    print("Connected:", cl.get_version().obs_version)
    tmp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "sr_repro")
    os.makedirs(tmp, exist_ok=True)
    for fn in (lambda: cl.remove_input(SRC), lambda: cl.remove_scene(SCENE)):
        try:
            fn()
        except Exception:
            pass
    cl.create_scene(SCENE)
    kinds = cl.get_input_kind_list(False).input_kinds
    ck = next((k for k in kinds if k.startswith("color_source")), "color_source_v3")
    cl.create_input(SCENE, SRC, ck, {"width": 640, "height": 360}, True)
    cl.create_source_filter(SRC, "SR_F", "source_record_filter", {"path": tmp, "record_mode": 0})
    time.sleep(1.5)  # let the tick create the private view + mix

    vs = cl.get_video_settings()
    print(f"base {vs.base_width}x{vs.base_height}")
    ok_b1 = False
    try:
        nw, nh = (1280, 720) if (vs.base_width, vs.base_height) != (1280, 720) else (1600, 900)
        cl.set_video_settings(vs.fps_numerator, vs.fps_denominator, nw, nh, nw, nh)  # -> obs_reset_video
        print(f"triggered obs_reset_video -> {nw}x{nh}")
        time.sleep(1.5)
        started = vendor(cl, "record_start", {"source": SRC})
        time.sleep(2)
        alive = cl.get_version().obs_version  # if OBS crashed this raises
        rec = cl.get_record_status().output_active
        print(f"B1: OBS alive after reset+record ({alive}); global record_active={rec}")
        ok_b1 = bool(alive) and started.get("success") is True and any(
            os.path.getsize(os.path.join(tmp, f)) > 0 for f in os.listdir(tmp))
        vendor(cl, "record_stop", {"source": SRC})
        time.sleep(1)
    finally:
        cl.set_video_settings(vs.fps_numerator, vs.fps_denominator, vs.base_width, vs.base_height, vs.output_width,
                              vs.output_height)
        print("video settings restored")

    time.sleep(2.5)  # let the idle tick release SOURCE_CHANNEL (B14)
    cl.remove_input(SRC)
    time.sleep(1.5)
    names = [i["inputName"] for i in cl.get_input_list().inputs]
    ok_b14 = SRC not in names
    print(f"B14: source removed cleanly, no ghost = {ok_b14}")
    try:
        cl.remove_scene(SCENE)
    except Exception:
        pass

    print()
    print(f"[{'PASS' if ok_b1 else 'FAIL'}] B1  reset+record without crash")
    print(f"[{'PASS' if ok_b14 else 'FAIL'}] B14 no ghost source after removal")
    return 0 if (ok_b1 and ok_b14) else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except IsolationError as error:
        print(f"ABORT: {error}", file=sys.stderr)
        sys.exit(2)
