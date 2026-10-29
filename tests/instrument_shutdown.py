"""Add a test-only barrier immediately before the UI unregister enqueue.

The production source is left untouched. Used to hold teardown while the UI
begins closing, then release it into the previously synchronous UI rendezvous.
"""
from pathlib import Path
import re
import sys

source = Path(sys.argv[1]).read_text()
barrier = r'''
static void sr_shutdown_barrier(void)
{
    const char *ready = getenv("SR_TEST_SHUTDOWN_READY");
    const char *release = getenv("SR_TEST_SHUTDOWN_RELEASE");
    if (!ready || !release)
        return;
    FILE *marker = os_fopen(ready, "wb");
    if (marker) fclose(marker);
    while (!os_file_exists(release)) os_sleep_ms(10);
}
'''
anchor = "static void remove_frontend_cb_task(void *data)"
assert source.count(anchor) == 1
source = source.replace(anchor, barrier + "\n" + anchor)
source, count = re.subn(r"(\t\t)(obs_queue_task\(OBS_TASK_UI, remove_frontend_cb_task,[^\n]+\));",
                      r"\1(sr_shutdown_barrier(), \2);", source)
assert count == 1, "Expected exactly one unregister enqueue"
Path(sys.argv[2]).write_text(source)
