#!/usr/bin/env python3
"""Rank Multivoid log failures without counting stack frames as new faults."""

import argparse
from bisect import bisect_right
from collections import Counter
import csv
import json
from pathlib import Path
import re
import sys


LINE = re.compile(r"^\[(\d\d:\d\d:\d\d(?:\.\d+)?)\]\s+\[(\w+)\s*\]\s*(.*)")
FRAME = re.compile(r"(?:caller\s+#\d+|stack\s*#?\d+|at\s+\S+\+0x)", re.I)
SEVERITY = {"critical": 0, "error": 1, "warning": 2}


def seconds(stamp):
    h, m, s = map(float, stamp.split(":"))
    return h * 3600 + m * 60 + s


def signature(message):
    # Keep exception codes, module RVAs, watch names and fingerprint keys intact.
    message = re.sub(r"\baccess=([0-9A-Fa-f]{16})\b", lambda m: f"access=0x{int(m[1], 16):X}", message)
    message = re.sub(r"\b(ip|modbase|self|actor|cycle|function|resumeFn)=([0-9A-Fa-f]{12,16})\b",
                     r"\1=<address>", message)
    message = re.sub(r"(?<![\w+])\b[0-9A-Fa-f]{16}\b", "<address>", message)
    message = re.sub(r"\(call #\d+\)|\(#\d+\)", "(#N)", message)
    message = re.sub(r"\b(?:pid|hConn)=\w+", lambda m: m[0].split("=")[0] + "=<id>", message)
    return message


def classify(level, message):
    low = message.lower()
    if re.search(r"\bFAULT\b|\b(?:fatal error|access violation)\b", message):
        return "fault", "critical"
    if (re.search(r"watch.*\bDEAD\b(?!\s*=\s*0\b)", message) or
            re.search(r"watch can never fire|watch settled dead", low)):
        return "dead-watch", "error"
    if "fingerprint-census:" in low and level in ("WARN", "WARNING", "ERROR", "FATAL") and re.search(
            r"sig-changed|body-changed|appeared|\bgone\b|undecodable|no-reference|refused|failed", low):
        return "fingerprint-drift", "error"
    if re.search(r"join failed|aborting the join|session abort|join.*\babort\b|host closed our connection", low):
        return "session-abort", "error"
    if re.search(r"\bdesync\b|\bdiverg(?:ed|ence)\b|verdict fail|synctest_err:.*\bfail\b|ctake-race: RESULT.*verdict=(?:duplicate|lost|aborted|fail)", message, re.I):
        return "desync", "error"
    if level in ("ERROR", "FATAL"):
        return "error", "error"
    if level in ("WARN", "WARNING"):
        return "warning", "warning"
    return None


class Findings:
    def __init__(self):
        self.rows = {}

    def add(self, category, severity, message, source, line, stamp=None):
        key = (category, signature(message))
        if key not in self.rows:
            self.rows[key] = dict(category=category, severity=severity, signature=key[1],
                                  count=0, sources={}, example=message, stacks=[])
        row = self.rows[key]
        row["count"] += 1
        ref = row["sources"].setdefault(source, dict(count=0, first_line=line,
                                                    last_line=line, first_time=stamp, last_time=stamp))
        ref.update(count=ref["count"] + 1, last_line=line, last_time=stamp)
        return row

    def ranked(self):
        return sorted(self.rows.values(), key=lambda r: (
            SEVERITY[r["severity"]], -r["count"], r["signature"]))


def read_log(path, role, findings):
    path = Path(path).resolve()
    meta = dict(path=str(path), role=role, pid=None, build=None, sha=None, first_time=None,
                last_time=None, records=0, stack_lines=0, nul_bytes=0,
                memory_samples=0, peak_working_set_bytes=None, liveness="unknown (log only)")
    events, active = [], None
    armed, verdicts, race_result = {}, set(), False
    clock, last_clock, day, session, live = 0, 0, 0, 0, False
    # Binary iteration tolerates truncated UTF-8 and the zero-filled tail of a killed writer.
    with path.open("rb") as stream:
        for number, raw in enumerate(stream, 1):
            meta["nul_bytes"] += raw.count(b"\0")
            text = raw.replace(b"\0", b"").decode("utf-8", errors="replace").strip()
            if text.startswith("process "):
                meta["pid"] = text[8:]
            match = LINE.match(text)
            if not match:
                if active is not None and FRAME.search(text):
                    frame = signature(text)
                    if frame not in active["stacks"]:
                        active["stacks"].append(frame)
                    meta["stack_lines"] += 1
                continue
            stamp, level, message = match.groups()
            clock = seconds(stamp)
            if clock < last_clock - 43200:
                day += 86400
            last_clock = clock
            clock += day
            meta["records"] += 1
            meta["first_time"] = meta["first_time"] or stamp
            meta["last_time"] = stamp
            if FRAME.search(message) and active is not None:
                frame = signature(message)
                if frame not in active["stacks"]:
                    active["stacks"].append(frame)
                meta["stack_lines"] += 1
                continue
            active = None
            if "boot: Multivoid " in message:
                meta["build"] = message.split("boot: ", 1)[1]
            sha = re.search(r"boot: build .*?sha (\w+)", message)
            if sha:
                meta["sha"] = sha[1]
            if "session started role=" in message:
                session += 1
                live = True
            if re.search(r"net: session ended|join FAILED|HOST CLOSED OUR CONNECTION", message):
                live = False
            events.append(dict(time=clock, message=message, line=number, session=session, live=live))
            arm = re.search(r"synctest_err: ARMED scenario=(\w+)", message)
            verdict = re.search(r"synctest_err: VERDICT (?:PASS|FAIL) scenario=(\w+)", message)
            if arm:
                armed[arm[1]] = number
            if verdict:
                verdicts.add(verdict[1])
            if "director/ctake-race: RESULT" in message:
                race_result = True
            kind = classify(level, message)
            if kind:
                active = findings.add(*kind, message, str(path), number, stamp)
            mem = re.search(r"(?:working[_ -]?set|ws)[_ -]?(bytes|mb|mib|gb|gib)?\s*[=:]\s*(\d+(?:\.\d+)?)\s*(MiB|MB|GiB|GB|bytes)?", message, re.I)
            if mem:
                unit = (mem[3] or mem[1] or "bytes").lower()
                size = int(float(mem[2]) * {"bytes": 1, "mb": 1024**2, "mib": 1024**2,
                                           "gb": 1024**3, "gib": 1024**3}[unit])
                memory_sample(meta, size, findings, str(path), number)
            if re.search(r"(?:process|peer).*\b(?:exited|dead)\b|alive\s*[=:]\s*(?:false|0)\b", message, re.I):
                meta["liveness"] = "exit observed; see log for intent"
                findings.add("liveness", "error", message, str(path), number, stamp)
    if not meta["records"]:
        findings.add("liveness", "error", "No timestamped records (empty or unsupported log)", str(path), 1)
    for scenario, number in armed.items():
        if scenario not in verdicts and not (scenario == "race" and race_result):
            findings.add("scenario-incomplete", "error", f"Scenario {scenario} armed without a final verdict", str(path), number)
    return meta, events


def memory_sample(meta, size, findings, source, line, limit=6 * 1024**3):
    meta["memory_samples"] += 1
    meta["peak_working_set_bytes"] = max(size, meta["peak_working_set_bytes"] or 0)
    if size > limit:
        findings.add("memory", "critical", "Working set exceeds 6 GiB", source, line)


def read_telemetry(path, logs, findings):
    """Optional rig CSV: pid, working_set_bytes, alive (true/false), time."""
    with Path(path).open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        if not {"pid", "working_set_bytes", "alive"} <= set(reader.fieldnames or []):
            raise ValueError("telemetry needs pid,working_set_bytes,alive CSV columns")
        for number, row in enumerate(reader, 2):
            meta = next((m for m in logs if str(m["pid"]) == row["pid"]), None)
            if meta is None:
                raise ValueError(f"telemetry PID {row['pid']} has no supplied log")
            memory_sample(meta, int(row["working_set_bytes"]), findings, str(path), number)
            alive = row["alive"].lower()
            if alive not in ("true", "false", "1", "0"):
                raise ValueError(f"invalid alive value on telemetry line {number}")
            meta["liveness"] = "alive at last sample" if alive in ("true", "1") else "dead at last sample"
            if alive in ("false", "0"):
                findings.add("liveness", "critical", "Rig observed process not alive", str(path), number)


def marker(message, host):
    # These lines prove an apply or a declared no-replay policy, not merely a queued receive.
    patterns = ([
        ("event", r"event_fire: broadcast \S+ '([^']+)'"),
        ("alarm", r"alarm_sync: (?:host broadcast|connect-snapshot -- sent) active=(\d)"),
    ] if host else [
        ("event", r"event_fire: client REPLAY \S+ '([^']+)'"),
        ("event-policy", r"event_fire: '(.*?)' NOT replayed"),
        ("alarm", r"alarm_sync: applied active=(\d)"),
    ])
    for lane, pattern in patterns:
        match = re.search(pattern, message)
        if match:
            return lane, match[1]
    weather = re.search(r"weather: (?:host broadcast|connect-broadcast slot=\d+ sent) flags=(0x[\dA-Fa-f]+) rain=([\d.]+)", message) if host else re.search(
        r"weather: applied flags 0x[\dA-Fa-f]+ -> (0x[\dA-Fa-f]+) flags2=0x[\dA-Fa-f]+ rain=([\d.]+)", message)
    if weather:
        return "weather", f"flags={int(weather[1], 16)} rain={float(weather[2]):.2f}"
    return None


def compare(host, clients, findings, grace):
    comparisons = []
    hm, he = host
    for cm, ce in clients:
        result = dict(client=cm["path"], checked=0, missing=0, status="skipped")
        comparisons.append(result)
        if not hm["sha"] or hm["sha"] != cm["sha"] or hm["build"] != cm["build"]:
            result["reason"] = "Different or unknown builds; historical logs cannot prove paired delivery"
            continue
        if not he or not ce:
            result["reason"] = "No timestamped records"
            continue
        lo, hi = max(he[0]["time"], ce[0]["time"]), min(he[-1]["time"], ce[-1]["time"])
        if hi - lo < grace:
            result["reason"] = "No overlapping observation window with apply grace"
            continue
        result.update(status="checked", reason="Same build and overlapping wall clocks; date/session pairing must be supplied correctly")
        # Require a world-ready milestone in this client's current session before demanding a live apply.
        ready = {}
        client_times = [c["time"] for c in ce]
        for e in ce:
            if re.search(r"rig: READY|world.ready.*(?:sent|complete)|CLIENT WORLD READY", e["message"], re.I):
                ready.setdefault(e["session"], e["time"])
        for e in he:
            wanted = marker(e["message"], True)
            if not wanted or not lo <= e["time"] <= hi - grace:
                continue
            index = bisect_right(client_times, e["time"]) - 1
            if index < 0 or not ce[index]["live"]:
                continue
            sess = ce[index]["session"]
            if sess not in ready or ready[sess] > e["time"]:
                continue
            result["checked"] += 1
            start = bisect_right(client_times, e["time"] - 1)
            end = bisect_right(client_times, e["time"] + grace)
            applied = any(c["session"] == sess and c["live"] and
                          marker(c["message"], False) in (wanted, ("event-policy", wanted[1])) for c in ce[start:end])
            if not applied:
                result["missing"] += 1
                findings.add("missing-apply", "error",
                             f"Host sent {wanted[0]}={wanted[1]} with no client apply within {grace:g}s ({Path(cm['path']).name})",
                             hm["path"], e["line"])
        if not result["checked"]:
            result.update(status="unproven", reason="No supported host send during a proven client world-ready window")
    return comparisons


def analyze(host, clients, telemetry=None, grace=30):
    findings = Findings()
    h = read_log(host, "host", findings)
    cs = [read_log(p, f"client{n}", findings) for n, p in enumerate(clients, 1)]
    logs = [h[0]] + [c[0] for c in cs]
    if telemetry:
        read_telemetry(telemetry, logs, findings)
    comparisons = compare(h, cs, findings, grace)
    ranked = findings.ranked()
    return dict(schema_version=1, logs=logs, comparisons=comparisons, findings=ranked,
                totals=dict(signatures=len(ranked), occurrences=sum(r["count"] for r in ranked),
                            by_category=dict(Counter({cat: sum(r["count"] for r in ranked if r["category"] == cat)
                                                      for cat in {r["category"] for r in ranked}}))),
                limitations=["Log EOF does not prove process death or health; use rig telemetry for memory/liveness.",
                             "Cross-peer checks cover EventFire, AlarmState and WeatherState markers after client world-ready.",
                             "Time-only format has no date or connection identity; supply logs from the same run."])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, type=Path)
    parser.add_argument("--client", required=True, action="append", type=Path)
    parser.add_argument("--json", type=Path, default=Path("synctest-summary.json"))
    parser.add_argument("--telemetry", type=Path)
    parser.add_argument("--grace-seconds", type=float, default=30)
    parser.add_argument("--top", type=int, default=15)
    parser.add_argument("--fail-on-error", action="store_true")
    args = parser.parse_args()
    if args.grace_seconds <= 0 or args.top < 1:
        parser.error("grace-seconds and top must be positive")
    try:
        report = analyze(args.host, args.client, args.telemetry, args.grace_seconds)
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    except (OSError, ValueError) as exc:
        parser.exit(2, f"analyze_sync_logs: {exc}\n")
    print(f"{report['totals']['occurrences']} occurrences, {report['totals']['signatures']} signatures; stack frames excluded")
    for n, row in enumerate(report["findings"][:args.top], 1):
        print(f"{n:2}. {row['severity'].upper():8} {row['count']:5} [{row['category']}] {row['signature'][:240]}")
    for comp in report["comparisons"]:
        print(f"Compare {Path(comp['client']).name}: {comp['status']}; checked={comp['checked']} missing={comp['missing']}; {comp['reason']}")
    for log in report["logs"]:
        print(f"{Path(log['path']).name}: memory samples={log['memory_samples']}, peak={log['peak_working_set_bytes']}; {log['liveness']}; stack lines={log['stack_lines']}")
    print(f"JSON: {args.json.resolve()}")
    return int(args.fail_on_error and any(r["severity"] != "warning" for r in report["findings"]))


if __name__ == "__main__":
    sys.exit(main())
