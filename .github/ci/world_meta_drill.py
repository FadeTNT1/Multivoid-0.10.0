#!/usr/bin/env python3
"""Build and run the 0.10.0 world-metadata reader against files written in the game's format."""

import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
MOD = ROOT / "src/votv-coop"


def main():
    work = ROOT / "local/tests/world-meta"
    work.mkdir(parents=True, exist_ok=True)
    (work / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.20)\n'
        'project(world_meta_test LANGUAGES C CXX)\n'
        f'add_executable(world_meta_test "{HERE.as_posix()}/world_meta_test.cpp"\n'
        f'    "{(MOD / "src/ue_wrap/engine/world_meta.cpp").as_posix()}"\n'
        f'    "{(MOD / "third_party/miniz/miniz.c").as_posix()}")\n'
        'target_compile_features(world_meta_test PRIVATE cxx_std_17)\n'
        'target_compile_definitions(world_meta_test PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)\n'
        f'target_include_directories(world_meta_test PRIVATE "{(MOD / "include").as_posix()}"\n'
        f'    "{(MOD / "third_party/miniz").as_posix()}")\n',
        encoding="utf-8",
    )
    build = work / "build"
    configure = ["cmake", "-S", str(work), "-B", str(build)]
    if os.name == "nt":
        vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
        major = subprocess.check_output(
            [str(vswhere), "-latest", "-products", "*", "-requires",
             "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationVersion"],
            text=True).strip().split(".")[0]
        year = {"16": "2019", "17": "2022", "18": "2026"}[major]
        configure += ["-G", f"Visual Studio {major} {year}", "-A", "x64"]
    subprocess.run(configure, check=True)
    subprocess.run(["cmake", "--build", str(build), "--config", "Release", "--parallel", "2"], check=True)
    exe = build / "Release/world_meta_test.exe" if os.name == "nt" else build / "world_meta_test"
    subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
