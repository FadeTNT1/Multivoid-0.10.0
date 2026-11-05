#!/usr/bin/env python3
"""Compile the production prop motion scan with isolated engine and coop boundaries, and run it."""

import os
from pathlib import Path
import re
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
INCLUDE = ROOT / "src/votv-coop/include"
SOURCE = ROOT / "src/votv-coop/src/coop/props/prop_motion_scan.cpp"
WORK = ROOT / "local/tests/prop-motion-scan"
# The real header the source needs: its own, the constants and pure rules under test.
REAL = ("coop/props/prop_motion_scan.h",)
# The value types it reads, without the object-reference machinery the real types header carries.
TYPES = """#pragma once
#include <cmath>
#include <cstdint>
namespace ue_wrap {
struct FVector { float X = 0.f, Y = 0.f, Z = 0.f; };
struct FRotator { float Pitch = 0.f, Yaw = 0.f, Roll = 0.f; };
inline float NormalizeAxis(float deg) {
    deg = std::fmod(deg, 360.f);
    if (deg > 180.f) deg -= 360.f;
    if (deg < -180.f) deg += 360.f;
    return deg;
}
}  // namespace ue_wrap
"""


def main():
    stubs = WORK / "stubs"
    if stubs.exists():
        shutil.rmtree(stubs)
    stubs.mkdir(parents=True)
    for name in re.findall(r'^#include "([^"]+)"', SOURCE.read_text(encoding="utf-8"), re.M):
        target = stubs / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text('#pragma once\n', encoding="utf-8")
    for name in REAL:
        target = stubs / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(INCLUDE / name, target)
    types = stubs / "ue_wrap/core/types.h"
    types.parent.mkdir(parents=True, exist_ok=True)
    types.write_text(TYPES, encoding="utf-8")
    (WORK / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.20)\n'
        'project(prop_motion_scan_test LANGUAGES CXX)\n'
        f'add_executable(prop_motion_scan_test "{HERE.as_posix()}/prop_motion_scan_test.cpp")\n'
        'target_compile_features(prop_motion_scan_test PRIVATE cxx_std_17)\n'
        f'target_include_directories(prop_motion_scan_test PRIVATE "{stubs.as_posix()}")\n'
        f'target_compile_definitions(prop_motion_scan_test PRIVATE PROP_MOTION_SCAN_SOURCE="{SOURCE.as_posix()}")\n',
        encoding="utf-8",
    )
    build = WORK / "build"
    configure = ["cmake", "-S", str(WORK), "-B", str(build)]
    if os.name == "nt":
        vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
        major = subprocess.check_output(
            [str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationVersion"], text=True).strip().split(".")[0]
        year = {"16": "2019", "17": "2022", "18": "2026"}[major]
        configure += ["-G", f"Visual Studio {major} {year}", "-A", "x64"]
    subprocess.run(configure, check=True)
    subprocess.run(["cmake", "--build", str(build), "--config", "Release"], check=True)
    exe = build / "Release/prop_motion_scan_test.exe" if os.name == "nt" else build / "prop_motion_scan_test"
    subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
