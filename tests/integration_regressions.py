"""End-to-end regressions, exclusively through a verified isolated runner."""
import array
import json
import math
import os
from pathlib import Path
import subprocess
import socket
import time
import sys

from isolated_obs import connect, verify_instance, IsolationError


from media_checks import decode, probe, stable, wait_for


def main():
    instance = verify_instance()
    root = Path(instance["root"])
    cl = connect()
    def request(kind, data=None):
        return cl.send(kind, data or {}, raw=True)
    def vendor(kind, data=None):
        response = request("CallVendorRequest", {"vendorName": "source-record", "requestType": kind,
                                                 "requestData": data or {}}).get("responseData", {})
        assert response.get("success") is True, (kind, response)
        return response
    media = root / "synthetic.mp4"
    subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30",
                    "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-t", "12", "-c:v", "libx264",
                    "-preset", "ultrafast", "-c:a", "aac", str(media)], check=True)
    request("CreateScene", {"sceneName": "SR_TEST"})
    request("CreateInput", {"sceneName": "SR_TEST", "inputName": "SR_MEDIA", "inputKind": "ffmpeg_source",
        "inputSettings": {"is_local_file": True, "local_file": str(media), "looping": True}, "sceneItemEnabled": True})
    request("SetCurrentProgramScene", {"sceneName": "SR_TEST"})
    request("SetInputAudioTracks", {"inputName": "SR_MEDIA", "inputAudioTracks": {str(i): True for i in range(1, 7)}})

    def add(name, directory, **settings):
        directory.mkdir(parents=True, exist_ok=True)
        values = {"path": str(directory), "filename_formatting": "collision", "rec_format": "mp4",
                  "record_mode": 0, "stream_mode": 0, "encoder": "obs_x264", "audio_encoder": "ffmpeg_aac",
                  "audio_bitrate": 128, "bitrate": 1500, "rate_control": "CBR", "keyint_sec": 1,
                  "preset": "ultrafast", "remove_after_record": False}
        values.update(settings)
        request("CreateSourceFilter", {"sourceName": "SR_MEDIA", "filterName": name,
                                      "filterKind": "source_record_filter", "filterSettings": values})
    def settings(name, values):
        request("SetSourceFilterSettings", {"sourceName": "SR_MEDIA", "filterName": name,
                                            "filterSettings": values, "overlay": True})
    def remove(name):
        request("RemoveSourceFilter", {"sourceName": "SR_MEDIA", "filterName": name})
    def files(directory, count=1, suffix="mp4"):
        return wait_for(lambda: (fs if len(fs := list(directory.glob(f"*.{suffix}"))) >= count and
                                 all(f.stat().st_size > 4096 for f in fs) else None), message="private files growing")

    # H1: both transitions in inactive, armed filters, followed by destruction.
    for before, after in ((-1, 0), (0, -1)):
        add("OWNERSHIP", root / "ownership", record_mode=3, different_audio=True, audio_track=before)
        time.sleep(.5)
        settings("OWNERSHIP", {"audio_track": after})
        time.sleep(.5)
        remove("OWNERSHIP")
    request("StartRecord")
    time.sleep(5)
    witness = Path(request("StopRecord")["outputPath"])
    stable([witness])
    decode(witness)
    samples = array.array("f")
    samples.frombytes(subprocess.run(["ffmpeg", "-v", "error", "-i", str(witness), "-map", "0:a:0", "-t", "2",
        "-ac", "1", "-ar", "8000", "-f", "f32le", "pipe:1"], check=True, capture_output=True).stdout)
    assert samples and math.sqrt(sum(x*x for x in samples)/len(samples)) > .001
    print("PASS H1: global audio survives both ownership transitions and removal", flush=True)

    # H2: an inactive existing encoder must pick up a new codec and settings.
    d = root / "audio-settings"
    add("AUDIO", d, record_mode=3, rec_format="mkv", audio_bitrate=96)
    time.sleep(.5)
    settings("AUDIO", {"audio_encoder": "ffmpeg_opus", "audio_bitrate": 192})
    time.sleep(.5)
    vendor("record_start", {"source": "SR_MEDIA", "filter": "AUDIO"})
    paths = files(d, suffix="mkv")
    time.sleep(3)
    vendor("record_stop", {"source": "SR_MEDIA", "filter": "AUDIO"})
    stable(paths)
    info = decode(paths[0])
    assert next(s for s in info["streams"] if s["codec_type"] == "audio")["codec_name"] == "opus"
    remove("AUDIO")
    print("PASS H2: codec change applied to an existing idle audio configuration", flush=True)

    # H4/H6: simultaneous identical names and global operation on two filters.
    d = root / "collisions"
    add("A", d)
    add("B", d)
    settings("A", {"record_mode": 1})
    settings("B", {"record_mode": 1})
    paths = files(d, 2)
    assert len(set(paths)) == 2
    vendor("record_stop", {})
    stable(paths)
    for path in paths:
        # A correctly stopped recording can contain only a few frames here.
        decode(path, minimum_duration=0)
    remove("A")
    remove("B")
    print("PASS H4/H6: distinct files and global stop of both filters", flush=True)

    # H3: updating replay naming must preserve accumulated replay and recording.
    d = root / "replay"
    add("REPLAY", d, filename_formatting="recording", record_mode=1, replay_buffer=True,
        replay_duration=20, replay_filename_formatting="old_replay")
    paths = files(d)
    time.sleep(7)
    vendor("replay_buffer_start", {"source": "SR_MEDIA", "filter": "REPLAY", "filename": "renamed_replay"})
    time.sleep(4)
    vendor("replay_buffer_save", {"source": "SR_MEDIA", "filter": "REPLAY"})
    replay = wait_for(lambda: next(iter(d.glob("renamed_replay*.mp4")), None), message="saved replay")
    wait_for(lambda: replay.stat().st_size > 4096, message="replay written")
    stable([replay])
    assert float(decode(replay)["format"]["duration"]) >= 8, "replay history was discarded on rename"
    vendor("replay_buffer_stop", {"source": "SR_MEDIA", "filter": "REPLAY"})
    vendor("record_stop", {"source": "SR_MEDIA", "filter": "REPLAY"})
    stable(paths)
    assert len(list(d.glob("recording*.mp4"))) == 1, "replay rename split the recording"
    decode(paths[0])
    remove("REPLAY")
    print("PASS H3: named replay rename preserves buffer and recording continuity", flush=True)

    # Split sessions must not share the muxer's filename namespace.
    d = root / "splits"
    add("SPLIT_A", d, split_file=True, max_time_sec=2)
    add("SPLIT_B", d, split_file=True, max_time_sec=2)
    settings("SPLIT_A", {"record_mode": 1})
    settings("SPLIT_B", {"record_mode": 1})
    time.sleep(8)
    vendor("record_stop", {})
    paths = wait_for(lambda: (fs if len(fs := list(d.glob("*.mp4"))) >= 6 else None), message="split segments")
    stable(paths)
    assert len({p.name.split("[sr-")[1].split("]")[0] for p in paths}) == 2
    for path in paths:
        decode(path, minimum_duration=0)
    remove("SPLIT_A")
    remove("SPLIT_B")
    print("PASS H6: concurrent split sessions stay in separate namespaces", flush=True)

    # H5: a failed RTMP connection must retry independently of a healthy file.
    d = root / "partial-failure"
    with socket.socket() as unused:
        unused.bind(("127.0.0.1", 0))
        refused_port = unused.getsockname()[1]
    add("HEALTHY", d, record_mode=1, filename_formatting="healthy")
    paths = files(d)
    before = paths[0].stat().st_size
    add("FAILED_STREAM", root / "stream", stream_mode=1,
        server=f"rtmp://127.0.0.1:{refused_port}/live", key="synthetic-test-only")
    time.sleep(8)
    assert paths[0].stat().st_size > before, "failed stream stalled healthy recording"
    log = (root / "obs.log").read_text()
    attempts = log.count(f"Connecting to RTMP URL rtmp://127.0.0.1:{refused_port}/live")
    assert 2 <= attempts <= 5, f"unexpected retry/backoff count: {attempts}"
    vendor("stream_stop", {"source": "SR_MEDIA", "filter": "FAILED_STREAM"})
    vendor("record_stop", {"source": "SR_MEDIA", "filter": "HEALTHY"})
    stable(paths)
    decode(paths[0])
    remove("FAILED_STREAM")
    remove("HEALTHY")
    print("PASS H5: failed stream retries independently while healthy recording grows", flush=True)

    request("RemoveInput", {"inputName": "SR_MEDIA"})
    print("PASS: real OBS integration finished", flush=True)


if __name__ == "__main__":
    try:
        main()
    except IsolationError as error:
        print(f"ABORT: {error}", file=sys.stderr)
        sys.exit(2)
