#!/usr/bin/env python3
"""Compile and run the sleeper rebuild plan (loot then rejoin: no duplication, no loss) with no engine."""

import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
INCLUDE = ROOT / "src/votv-coop/include"
WORK = ROOT / "local/tests/sleeper-rebuild-plan"


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    (WORK / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.20)\n'
        'project(sleeper_rebuild_plan_test LANGUAGES CXX)\n'
        f'add_executable(sleeper_rebuild_plan_test "{HERE.as_posix()}/sleeper_rebuild_plan_test.cpp")\n'
        'target_compile_features(sleeper_rebuild_plan_test PRIVATE cxx_std_17)\n'
        f'target_include_directories(sleeper_rebuild_plan_test PRIVATE "{INCLUDE.as_posix()}")\n',
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
    exe = build / "Release/sleeper_rebuild_plan_test.exe" if os.name == "nt" else build / "sleeper_rebuild_plan_test"
    subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
