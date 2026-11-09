#!/usr/bin/env python3
"""Compile the production desk component-to-station mapping and run it against the 0.10.0 component names."""

import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
INCLUDE = ROOT / "src/votv-coop/include"
WORK = ROOT / "local/tests/desk-station"


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    exe = WORK / ("desk_station_test.exe" if os.name == "nt" else "desk_station_test")
    compiler = shutil.which("g++") or shutil.which("c++") or shutil.which("clang++")
    if compiler:
        cmd = [compiler, "-std=c++17", "-Wall", "-Wextra", f"-I{INCLUDE}", str(HERE / "desk_station_test.cpp"), "-o", str(exe)]
    else:
        cmd = ["cl", "/nologo", "/std:c++17", "/EHsc", f"/I{INCLUDE}", str(HERE / "desk_station_test.cpp"), f"/Fe:{exe}"]
    subprocess.run(cmd, check=True, cwd=WORK)
    subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
