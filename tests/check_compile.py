"""Compile-check the complete plugin using a pinned OBS source tree, without OBS."""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
version = re.search(r'project\(source-record VERSION ([0-9.]+)\)', (root/'CMakeLists.txt').read_text()).group(1)
assert version == json.loads((root/'buildspec.json').read_text())['version'], 'CMake/buildspec version mismatch'
if len(sys.argv) == 1:
    print(f'Version alignment: {version} PASS')
else:
    obs = Path(sys.argv[1]).resolve()
    frontend = next(p for p in (obs/'frontend/api', obs/'UI/obs-frontend-api') if (p/'obs-frontend-api.h').exists())
    with tempfile.TemporaryDirectory(prefix='sr-compile-') as d:
        include = Path(d)
        (include/'obsconfig.h').write_text('#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n')
        (include/'version.h').write_text(f'#define PROJECT_VERSION "{version}"\n')
        subprocess.run(['clang', '-fsyntax-only', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        f'-I{include}', f'-I{obs / "libobs"}', f'-I{frontend}', str(root/'source-record.c')], check=True)
    print(f'Complete plugin compile check: {obs.name} PASS')
