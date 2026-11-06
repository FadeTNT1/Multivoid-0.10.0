#!/usr/bin/env python3
"""script_gate_capacity_gate -- the script gate's tables can hold every watch the mod can register.

The script gate (src/votv-coop/src/ue_wrap/core/script_gate_detail.h) keeps its watches in fixed tables of
kSlots = 1 << kSlotBits slots, capped at kMaxKeyed = 3/8 of them. A watch past the cap is refused, and a
refused watch is a gate that never fires: in b288 a table that held 384 name watches took 64 of them, and
every door, light switch, locker, keypad and poster lane went dead with it.

Every game-function row (`UE_FACT_FN(...)`) is at most one watch per tag that registers it, and nearly
every row is registered once, so the row count is the bound this gate holds against the cap: the rows must
fit inside three quarters of kMaxKeyed (the mark past which the gate's own settle summary is an error), so
a growing watch set fails here before it fails in a game. The run-time truth is the summary line
"script_gate: N/M watches live, 0 dead" the gate logs once its names resolve.

  python .github/ci/script_gate_capacity_gate.py           # PASS/FAIL over the tree
  python .github/ci/script_gate_capacity_gate.py --drill   # the gate must fail a planted small table
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
MOD = ROOT / "src" / "votv-coop"
DETAIL = MOD / "src" / "ue_wrap" / "core" / "script_gate_detail.h"
ROW = re.compile(r"\bUE_FACT_FN\s*\(")
SLOT_BITS = re.compile(r"constexpr\s+int\s+kSlotBits\s*=\s*(\d+)\s*;")


def count_rows(root):
    n = 0
    for p in root.rglob("*.cpp"):
        if "third_party" in p.parts:
            continue
        n += len(ROW.findall(p.read_text(encoding="utf-8", errors="replace")))
    return n


def judge(detail_text, rows):
    m = SLOT_BITS.search(detail_text)
    if not m:
        return False, "kSlotBits not found in script_gate_detail.h"
    slots = 1 << int(m.group(1))
    max_keyed = slots * 3 // 8
    warn = max_keyed * 3 // 4
    ok = rows <= warn
    return ok, (f"{rows} fact row(s) against {warn} (three quarters of the {max_keyed}-slot cap of a "
                f"{slots}-slot table)")


def main(argv):
    rows = count_rows(MOD)
    detail = DETAIL.read_text(encoding="utf-8")
    if "--drill" in argv:
        planted = SLOT_BITS.sub("constexpr int kSlotBits = 10;", detail)
        ok, why = judge(planted, rows)
        if ok:
            print(f"script_gate_capacity_gate: DRILL FAIL -- a 1024-slot table passed: {why}")
            return 1
        print(f"script_gate_capacity_gate: DRILL PASS -- the planted 1024-slot table fails: {why}")
        return 0
    ok, why = judge(detail, rows)
    print(f"script_gate_capacity_gate: {'PASS' if ok else 'FAIL'} -- {why}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
