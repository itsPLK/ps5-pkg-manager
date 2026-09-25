#!/usr/bin/env python3
"""Apply the tracked libsmb2 patches to a build copy; leave the submodule intact."""
import pathlib
import shutil
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parent.parent
dest = pathlib.Path(sys.argv[1]).resolve()
source = root / "deps" / "libsmb2"
if dest == source or source in dest.parents or dest in source.parents:
    raise SystemExit("The build copy must be separate from the libsmb2 submodule")
dest.mkdir(parents=True, exist_ok=True)
shutil.copytree(source, dest, dirs_exist_ok=True,
                ignore=shutil.ignore_patterns(".git", "build", "__pycache__"))
patches = root / "patches" / "libsmb2"
for patch in sorted(patches.glob("*.patch")):
    subprocess.run(["patch", "--batch", "-p1", "-i", str(patch)], cwd=dest, check=True)
shutil.copy2(patches / "cmac_accel.h", dest / "lib" / "cmac_accel.h")
(dest / ".prepared").touch()
