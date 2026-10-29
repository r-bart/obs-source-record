"""Force unregister-versus-UI-shutdown ordering in a test-only plugin build."""
from pathlib import Path
import threading
import time

from isolated_obs import connect, verify_instance, stop_owned
from media_checks import wait_for


def main():
    instance = verify_instance()
    root = Path(instance["root"])
    cl = connect()
    cl.create_scene("SHUTDOWN_RACE")
    cl.create_input("SHUTDOWN_RACE", "RACE_INPUT", "color_source_v3",
                    {"width": 320, "height": 240}, True)
    cl.create_source_filter("RACE_INPUT", "RACE_FILTER", "source_record_filter", {"record_mode": 0, "stream_mode": 0})
    time.sleep(.5)
    def remove():
        try:
            removal = connect()
            removal.remove_input("RACE_INPUT")
            removal.remove_scene("SHUTDOWN_RACE")
        except Exception as error:
            # Shutdown can close the test client's socket while it removes.
            print(f"Removal client ended during shutdown: {type(error).__name__}", flush=True)
    worker = threading.Thread(target=remove, daemon=True)
    worker.start()
    wait_for(lambda: (root / "cleanup-ready").is_file(), timeout=10, message="teardown at unregister barrier")
    def release():
        wait_for(lambda: "Shutting down" in (root / "obs.log").read_text(), timeout=10, message="UI starts closing")
        time.sleep(.5)  # Let UI reach its destruction-queue wait.
        (root / "cleanup-release").touch()
    releaser = threading.Thread(target=release, daemon=True)
    releaser.start()
    assert stop_owned(timeout=10), "UI and teardown deadlocked after releasing unregister barrier"
    releaser.join(timeout=2)
    worker.join(timeout=2)
    print("PASS: UI shutdown completes when unregister resumes behind the UI's teardown wait", flush=True)


if __name__ == "__main__":
    main()
