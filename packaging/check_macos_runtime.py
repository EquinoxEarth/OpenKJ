#!/usr/bin/env python3
"""Run the media smoke test using only the runtime in an app or DMG."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from relocate_macos import external, otool_refs


def run(*args, **kwargs):
    subprocess.run(args, check=True, **kwargs)


def check(app, smoke, work):
    contents = app.resolve() / "Contents"
    work.mkdir()
    (work / "Frameworks").symlink_to(contents / "Frameworks", target_is_directory=True)
    (work / "MacOS").mkdir()
    probe = work / "MacOS" / "runtime-smoke"
    shutil.copy2(smoke, probe)
    for dep in otool_refs(probe):
        if external(dep):
            name = Path(dep).name
            if ".framework/" in dep:
                name = dep.split("/lib/")[-1]
            if not (work / "Frameworks" / name).is_file():
                raise RuntimeError(f"Runtime probe dependency missing from app: {dep}")
            run("install_name_tool", "-change", dep, "@loader_path/../Frameworks/" + name, probe)
    run("codesign", "--force", "--sign", "-", probe)

    fixtures = work / "fixtures"
    fixtures.mkdir()
    run("ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin",
        "-f", "lavfi", "-i", "testsrc2=size=160x120:rate=25",
        "-f", "lavfi", "-i", "sine=frequency=440", "-t", "0.4",
        "-c:v", "libx264", "-c:a", "aac", fixtures / "karaoke.mp4")
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("DYLD_", "GST_", "QT_"))}
    env.update(
        GST_PLUGIN_SYSTEM_PATH_1_0=str(contents / "PlugIns"),
        GST_PLUGIN_SCANNER=str(contents / "Helpers" / "gst-plugin-scanner"),
        GST_REGISTRY_1_0=str(work / "registry.bin"),
        QT_PLUGIN_PATH=str(contents / "PlugIns"),
        QT_QPA_PLATFORM="cocoa",
    )
    run(probe, "--bundled-macos", "--media-directory", fixtures, env=env, timeout=90)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("dmg", type=Path)
    parser.add_argument("--smoke", type=Path, default=Path("build-macos-dev/openkj-runtime-smoke"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="openkj-package-check-") as temporary:
        work = Path(temporary)
        check(args.app, args.smoke, work / "assembled")
        mount = work / "mounted"
        mount.mkdir()
        run("hdiutil", "attach", "-readonly", "-nobrowse", "-mountpoint", mount, args.dmg)
        try:
            app = mount / "OpenKJ.app"
            run("codesign", "--verify", "--deep", "--strict", app)
            check(app, args.smoke, work / "dmg")
        finally:
            run("hdiutil", "detach", mount)


if __name__ == "__main__":
    main()
