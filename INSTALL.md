# Installing & updating this fork

This is a hardened fork of [exeldro/obs-source-record](https://github.com/exeldro/obs-source-record)
with extra bug fixes. Prebuilt installers are published as **GitHub Releases** and
built by CI for Windows, macOS and Linux.

## Install

1. Go to **[Releases](https://github.com/r-bart/obs-source-record/releases)** and open the latest.
2. Download the file for your OS:
   - **Windows** — `source-record-…-windows-installer` → unzip → run `source-record-installer.exe`.
     (Unsigned, so SmartScreen warns: *More info → Run anyway*.)
   - **macOS** — `source-record-…-macos-universal.pkg` → double-click → install.
     (Ad-hoc signed: if Gatekeeper blocks it, right-click → **Open**, or run
     `xattr -dr com.apple.quarantine <file>.pkg` first.)
   - **Linux** — `source-record-…-ubuntu-….tar.gz` → extract into your OBS plugins dir
     (`~/.config/obs-studio/plugins/`).
3. Restart OBS. Confirm in the log: `[Source Record] loaded version 0.5.0`.

## Update

Same as install — download the newest Release and reinstall over the top. OBS shows
the loaded version in its log so you can tell what you're running.

## Cutting a new release (maintainer)

When new fixes land on `develop`:

```bash
# 1. bump CMakeLists.txt AND buildspec.json to the same version
python3 tests/check_compile.py
git commit -am "bump to 0.5.x" && git push fork develop

# 2. tag it
git tag -a v0.5.x -m "source-record 0.5.x" && git push fork v0.5.x

# 3. the tag push triggers build + publication automatically
# Monitor the run in Actions; do not dispatch a second build on the tag.
```

After all platform builds pass, the workflow publishes the GitHub Release.
The regression workflow also runs on pushes and pull requests. Version and
regression checks must pass before the release workflow builds its installers. It does not install
or update your running OBS. Existing release assets are never overwritten by a retry.

Ad-hoc / on-demand binaries without a release: `gh workflow run build.yml --repo
r-bart/obs-source-record --ref develop`, then download from the run's Artifacts.

## Regression tests

Run the tests without OBS:

```bash
python3 -m unittest discover -s tests -p 'test_*.py' -v
```

The C fixtures compile the production functions with an instrumented adapter and
ASan/UBSan. They cover ownership, callbacks, cancellation, retries, filenames and
API selection; they do not replace integration against real libobs.

Run integration only on a dedicated Linux runner/VM via
`tests/isolated_obs.py`, with an explicit test plugin binary. All clients require
its manifest and verify process/configuration/socket ownership. Ambient
`OBS_WS_*` credentials and attaching to your personal OBS are disabled. Shutdown
controls only the process owned by the runner. macOS/Windows integration remains
pending until equivalent isolation is verified.

With file splitting enabled, filenames include a session suffix so concurrent
muxers have separate namespaces for subsequent segments. Settings retain the
user's original filename pattern. Audio settings changed during recording or
replay remain pending until all outputs stop; opening properties shows this state.

WebSocket operations without `source` address every matching Source Record filter
once. `source` without `filter` retains the existing single-filter selection;
`source` + `filter` selects that named filter. A successful asynchronous request
means accepted settings, not a guarantee that the encoder or destination started.
