"""Build a test DLL with MSVC and headers from an explicit OBS source tree.

Import libraries are generated from the supplied OBS binaries. No installation
files are written. Requires the Visual Studio C++ tools and Windows SDK.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--obs-source", type=Path, required=True)
    parser.add_argument("--obs-install", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source, install, out = args.obs_source.resolve(), args.obs_install.resolve(), args.output.resolve()
    if out.is_relative_to(install) or out.is_relative_to(source):
        raise RuntimeError("Build output must be separate from OBS installation and sources")
    out.mkdir(parents=True, exist_ok=True)
    vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    vs = Path(subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip())
    versions = vs / "VC/Tools/MSVC"
    version = max(versions.iterdir(), key=lambda p: tuple(map(int, p.name.split("."))))
    tools = version / "bin/Hostx64/x64"
    for name in ("obs", "obs-frontend-api", "w32-pthreads"):
        raw = subprocess.check_output([str(tools / "dumpbin.exe"), "/nologo", "/exports",
                                       str(install / f"bin/64bit/{name}.dll")], text=True, errors="replace")
        exports = re.findall(r"^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)", raw, re.M)
        if not exports:
            raise RuntimeError(f"No exports found in {name}")
        definition = out / f"{name}.def"
        definition.write_text(f"LIBRARY {name}.dll\nEXPORTS\n" + "\n".join(exports))
        subprocess.run([str(tools / "lib.exe"), "/nologo", f"/def:{definition}", "/machine:x64",
                        f"/out:{out / name}.lib"], check=True)
    version_text = re.search(r"project\(source-record VERSION ([0-9.]+)\)", (root / "CMakeLists.txt").read_text())[1]
    values = dict(zip(("PROJECT_VERSION_MAJOR", "PROJECT_VERSION_MINOR", "PROJECT_VERSION_PATCH"), version_text.split(".")))
    values.update(PROJECT_VERSION=version_text, PROJECT_FULL_NAME="Source Record", PROJECT_NAME="source-record")
    for template, destination in (("version.h.in", "version.h"), ("resource.rc.in", "source-record.rc")):
        text = (root / template).read_text()
        for key, value in values.items():
            text = text.replace("${" + key + "}", value)
        (out / destination).write_text(text)
    (out / "obsconfig.h").write_text("#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")
    args = ["cl", "/nologo", "/LD", "/MD", "/Zi", "/O2", "/std:c11", "/W3", "/D_CRT_SECURE_NO_WARNINGS",
            *[f"/I{p}" for p in (out, source / "libobs", source / "frontend/api", source / "deps/w32-pthreads")],
            str(root / "source-record.c"), str(out / "source-record.res"), f"/Fo{out}/", f"/Fd{out}/compile.pdb",
            "/link", f"/OUT:{out}/source-record.dll", f"/PDB:{out}/source-record.pdb",
            *[str(out / f"{name}.lib") for name in ("obs", "obs-frontend-api", "w32-pthreads")]]
    command = out / "compile.cmd"
    command.write_text("@echo off\ncall " + subprocess.list2cmdline([str(vs / "VC/Auxiliary/Build/vcvars64.bat")]) +
        " >nul\nif errorlevel 1 exit /b %errorlevel%\n" +
        subprocess.list2cmdline(["rc", "/nologo", f"/fo{out}/source-record.res", str(out / "source-record.rc")]) +
        "\nif errorlevel 1 exit /b %errorlevel%\n" + subprocess.list2cmdline(args) + "\n")
    subprocess.run(["cmd", "/d", "/c", str(command)], check=True, cwd=out)
    (out / "build-manifest.json").write_text(json.dumps({
        "source_sha256": hashlib.sha256((root / "source-record.c").read_bytes()).hexdigest(),
        "dll_sha256": hashlib.sha256((out / "source-record.dll").read_bytes()).hexdigest(),
        "obs_source": str(source), "obs_install": str(install), "msvc": str(version), "optimization": "/O2",
    }, indent=2))


if __name__ == "__main__":
    main()
