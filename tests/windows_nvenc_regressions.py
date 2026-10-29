"""Synthetic NVENC codec, churn and active-shutdown tests in the Windows runner."""
import json
import os
from pathlib import Path
import subprocess
import time

from isolated_obs import connect, verify_instance
from media_checks import decode, stable, wait_for


def main():
    instance = verify_instance()
    assert instance["platform"] == "windows"
    root = Path(instance["root"])
    cl = connect()
    assert cl.get_version().obs_version == "33.0.0"
    media = root / "synthetic.mp4"
    subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=60",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-t", "12", "-c:v", "libx264",
        "-preset", "ultrafast", "-c:a", "aac", str(media)], check=True, timeout=30)
    cl.create_scene("NVENC_TEST")
    cl.create_input("NVENC_TEST", "NVENC_MEDIA", "ffmpeg_source",
                    {"is_local_file": True, "local_file": str(media), "looping": True}, True)
    cl.set_current_program_scene("NVENC_TEST")

    def add(name, encoder, directory, **extra):
        directory.mkdir(exist_ok=True)
        settings = {"path": str(directory), "filename_formatting": name, "record_mode": 1,
            "stream_mode": 0, "rec_format": "mkv", "encoder": encoder, "audio_encoder": "ffmpeg_aac",
            "audio_bitrate": 128, "bitrate": 4000, "rate_control": "CBR", "keyint_sec": 1,
            "remove_after_record": False}
        settings.update(extra)
        cl.create_source_filter("NVENC_MEDIA", name, "source_record_filter", settings)
        return wait_for(lambda: next((p for p in directory.glob("*.mkv") if p.stat().st_size > 4096), None),
                        message=f"{encoder} recording grows")

    case = os.environ.get("SR_WINDOWS_CASE", "codecs-and-churn")
    if case == "active-close":
        path = add("ACTIVE_CLOSE", "obs_nvenc_h264_tex", root / "active-close",
                   replay_buffer=True, replay_duration=10)
        time.sleep(3)
        (root / "validate-after-close.json").write_text(json.dumps({"files": [{"path": str(path), "codec": "h264"}]}))
        print("Active NVENC recording and replay left running for normal WM_CLOSE shutdown", flush=True)
        return

    for name, codec in (("obs_nvenc_h264_tex", "h264"), ("obs_nvenc_hevc_tex", "hevc"), ("obs_nvenc_av1_tex", "av1")):
        path = add(codec.upper(), name, root / codec)
        time.sleep(3)
        cl.remove_source_filter("NVENC_MEDIA", codec.upper())
        stable([path])
        info = decode(path)
        assert next(s for s in info["streams"] if s["codec_type"] == "video")["codec_name"] == codec
        print(f"PASS NVENC {codec}: finalized, decoded video and audio", flush=True)

    witness = add("WITNESS", "obs_nvenc_h264_tex", root / "witness")
    before = witness.stat().st_size
    for i in range(12):
        directory = root / f"churn-{i}"
        path = add(f"CHURN_{i}", "obs_nvenc_h264_tex", directory, replay_buffer=True, replay_duration=5)
        time.sleep(.25)
        cl.remove_source_filter("NVENC_MEDIA", f"CHURN_{i}")
        stable([path])
        decode(path, minimum_duration=0)
    assert witness.stat().st_size > before, "filter churn stalled the witness recording"
    cl.remove_source_filter("NVENC_MEDIA", "WITNESS")
    stable([witness])
    info = decode(witness)
    duration = float(info["format"]["duration"])
    assert duration >= 30, duration
    print(f"PASS 12 NVENC record/replay removal cycles; witness decoded ({duration:.1f}s)", flush=True)
    cl.remove_input("NVENC_MEDIA")
    cl.remove_scene("NVENC_TEST")


if __name__ == "__main__":
    main()
