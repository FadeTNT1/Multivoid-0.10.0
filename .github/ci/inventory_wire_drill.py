#!/usr/bin/env python3
"""Compile the production per-player profile codec against stand-in byte primitives and run it."""

import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
MOD = ROOT / "src/votv-coop"
SOURCE = MOD / "src/coop/items/inventory_wire.cpp"
WORK = ROOT / "local/tests/inventory-wire"

# The primitives' header without the save-record half, whose engine types the envelope never names.
PRIMITIVES = """#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace coop::save_record_wire {
bool Feasible(uint32_t n, const std::vector<uint8_t>& b, size_t o);
void AppU8(std::vector<uint8_t>& b, uint8_t v);
void AppU32(std::vector<uint8_t>& b, uint32_t v);
void AppF32(std::vector<uint8_t>& b, float v);
void AppWStr(std::vector<uint8_t>& b, const std::wstring& s);
bool RdU8(const std::vector<uint8_t>& b, size_t& o, uint8_t& v);
bool RdU32(const std::vector<uint8_t>& b, size_t& o, uint32_t& v);
bool RdF32(const std::vector<uint8_t>& b, size_t& o, float& v);
bool RdWStr(const std::vector<uint8_t>& b, size_t& o, std::wstring& s);
}
"""


def main():
    stubs = WORK / "stubs"
    (stubs / "coop/items").mkdir(parents=True, exist_ok=True)
    (stubs / "coop/items/save_record_wire.h").write_text(PRIMITIVES, encoding="utf-8")
    (WORK / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.20)\n'
        'project(inventory_wire_test LANGUAGES CXX)\n'
        f'add_executable(inventory_wire_test "{HERE.as_posix()}/inventory_wire_test.cpp" "{SOURCE.as_posix()}")\n'
        'target_compile_features(inventory_wire_test PRIVATE cxx_std_17)\n'
        f'target_include_directories(inventory_wire_test BEFORE PRIVATE "{stubs.as_posix()}")\n'
        f'target_include_directories(inventory_wire_test PRIVATE "{(MOD / "include").as_posix()}")\n',
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
    subprocess.run(["cmake", "--build", str(build), "--config", "Release", "--parallel", "1"], check=True)
    exe = build / "Release/inventory_wire_test.exe" if os.name == "nt" else build / "inventory_wire_test"
    subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
