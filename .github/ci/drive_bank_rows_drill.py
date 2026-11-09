#!/usr/bin/env python3
"""Compile and run the drive-box row sidecar round trip with no engine.

The pure pack/plan arithmetic (ue_wrap/actors/drive_bank_rows.h) is driven through the production record
wire (save_record_wire.cpp and signal_wire.cpp), with only the log and the blob-chunk limits stubbed.
"""

import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
MOD = ROOT / "src/votv-coop"
WORK = ROOT / "local/tests/drive-bank-rows"

LOG_STUB = """#pragma once
#define UE_LOGI(...) ((void)0)
#define UE_LOGW(...) ((void)0)
#define UE_LOGE(...) ((void)0)
"""

BLOB_STUB = """#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
namespace coop::blob_chunks {
inline size_t MaxBlobBytes() { return 1u << 20; }
inline constexpr size_t kLaneBlobBytes = 1u << 20;
inline uint64_t Fnv64(const std::vector<uint8_t>& b) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : b) { h ^= c; h *= 1099511628211ull; }
    return h;
}
}
"""


def main():
    stubs = WORK / "stubs"
    (stubs / "ue_wrap/core").mkdir(parents=True, exist_ok=True)
    (stubs / "coop/net").mkdir(parents=True, exist_ok=True)
    (stubs / "ue_wrap/core/log.h").write_text(LOG_STUB, encoding="utf-8")
    (stubs / "coop/net/blob_chunks.h").write_text(BLOB_STUB, encoding="utf-8")
    (WORK / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.20)\n'
        'project(drive_bank_rows_test LANGUAGES CXX)\n'
        'add_executable(drive_bank_rows_test\n'
        f'    "{HERE.as_posix()}/drive_bank_rows_test.cpp"\n'
        f'    "{(MOD / "src/coop/items/save_record_wire.cpp").as_posix()}"\n'
        f'    "{(MOD / "src/coop/interactables/signal_wire.cpp").as_posix()}")\n'
        'target_compile_features(drive_bank_rows_test PRIVATE cxx_std_20)\n'
        # The stubs come first so the two engine-bound headers resolve to them.
        f'target_include_directories(drive_bank_rows_test PRIVATE "{stubs.as_posix()}" "{(MOD / "include").as_posix()}")\n',
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
    exe = build / "Release/drive_bank_rows_test.exe" if os.name == "nt" else build / "drive_bank_rows_test"
    subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
