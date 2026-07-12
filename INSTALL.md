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
# 1. bump the version in CMakeLists.txt:  project(source-record VERSION 0.5.x)
git commit -am "bump to 0.5.x" && git push fork develop

# 2. tag it
git tag -a v0.5.x -m "source-record 0.5.x" && git push fork v0.5.x

# 3. build + publish the Release (fork Actions only run on manual dispatch, so
#    dispatch the workflow ON THE TAG — that sets github.ref to the tag and fires
#    the release job)
gh workflow run build.yml --repo r-bart/obs-source-record --ref v0.5.x
```

~8 minutes later a GitHub Release with all three installers appears. It's free
(public repo). Nothing runs on ordinary pushes, so day-to-day commits cost no
Actions minutes.

Ad-hoc / on-demand binaries without a release: `gh workflow run build.yml --repo
r-bart/obs-source-record --ref develop`, then download from the run's Artifacts.
