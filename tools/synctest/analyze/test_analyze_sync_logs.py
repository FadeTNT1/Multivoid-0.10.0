"""Engine-free regressions for fault grouping and paired-log evidence."""

import json
from pathlib import Path
import tempfile
import unittest

from analyze_sync_logs import analyze


BOOT = "process 11\n[12:00:00] [INFO ] boot: Multivoid 0.10.0 b271\n[12:00:00] [INFO ] boot: build unofficial sha abcdef12\n"


class AnalyzerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def log(self, name, text):
        path = self.root / name
        path.write_text(text, encoding="utf-8")
        return path

    def test_fault_frames_count_once_and_keep_rva(self):
        fault = "[12:00:01] [ERROR] game_thread: posted task FAULT code=0xC0000005 ip=00007FFF12345678 [main.dll+0x1234 modbase=00007FFF10000000] access=0000000000000002 task=PostMenuTick\n"
        stack = "[12:00:01] [ERROR] game_thread:   caller #0 main.dll+0xABCD modbase=00007FFF10000000\n"
        host = self.log("host.log", BOOT + (fault + stack) * 3 + "\0" * 100)
        client = self.log("client.log", BOOT)
        report = analyze(host, [client])
        row = report["findings"][0]
        self.assertEqual((row["count"], len(row["stacks"])), (3, 1))
        self.assertIn("main.dll+0x1234", row["signature"])
        self.assertIn("0xC0000005", row["signature"])
        self.assertEqual(report["logs"][0]["stack_lines"], 3)
        self.assertEqual(report["logs"][0]["nul_bytes"], 100)
        json.dumps(report)

    def test_dead_drift_abort_and_info_desync(self):
        host = self.log("host.log", BOOT + "\n".join([
            "[12:00:01] [WARN ] prop_act: watch mainPlayer_C.take DEAD",
            "[12:00:02] [WARN ] fingerprint-census: body-changed a.b (feature x)",
            "[12:00:03] [WARN ] join_progress: join FAILED [MV-T01] -- aborting the join",
            "[12:00:04] [INFO ] synctest_err: VERDICT FAIL scenario=race",
        ]))
        report = analyze(host, [self.log("client.log", BOOT)])
        self.assertEqual(set(report["totals"]["by_category"]),
                         {"dead-watch", "fingerprint-drift", "session-abort", "desync"})

    def test_normal_dead_cleanup_is_not_a_dead_watch(self):
        host = self.log("host.log", BOOT + "[12:00:01] [INFO ] gates: live=2 DEAD=0\n" +
                        "[12:00:02] [INFO ] prop_element_tracker: reaped 256 dead local Prop Element(s)\n" +
                        "[12:00:03] [INFO ] fingerprint-census: 16 gone before they were met, 0 still loading\n" +
                        "[12:00:04] [INFO ] script_gate: 1 class-scoped watch(es) judged: 0 dead; 1 still wait\n")
        self.assertEqual(analyze(host, [self.log("client.log", BOOT)])["findings"], [])

    def paired(self, client_extra="", host_extra="", client_boot=BOOT):
        host = self.log("host.log", BOOT + "[12:00:05] [INFO ] net: session started role=host\n" +
                        "[12:00:10] [INFO ] event_fire: broadcast runEvent 'piramid'\n" + host_extra +
                        "[12:01:10] [INFO ] heartbeat\n")
        client = self.log("client.log", client_boot +
                          "[12:00:05] [INFO ] net: session started role=client\n" +
                          "[12:00:06] [INFO ] rig: READY local-world-ready\n" + client_extra +
                          "[12:01:10] [INFO ] heartbeat\n")
        return analyze(host, [client])

    def test_missing_apply_and_success(self):
        missing = self.paired()
        self.assertEqual(missing["comparisons"][0]["missing"], 1)
        ok = self.paired("[12:00:11] [INFO ] event_fire: client REPLAY runEvent 'piramid'\n")
        self.assertEqual(ok["comparisons"][0]["missing"], 0)

    def test_queue_is_not_apply_and_policy_is_explicit(self):
        queued = self.paired("[12:00:11] [INFO ] event_fire: queued 'piramid' (1 pending)\n")
        self.assertEqual(queued["comparisons"][0]["missing"], 1)
        excluded = self.paired("[12:00:11] [INFO ] event_fire: 'piramid' NOT replayed (world actor)\n")
        self.assertEqual(excluded["comparisons"][0]["missing"], 0)

    def test_different_builds_never_claim_desync(self):
        report = self.paired(client_boot=BOOT.replace("abcdef12", "99999999"))
        self.assertEqual(report["comparisons"][0]["status"], "skipped")

    def test_weather_apply_matches_values(self):
        send = "[12:00:12] [INFO ] weather: host broadcast flags=0x01 rain=0.50 lc=1.00\n"
        receive = "[12:00:13] [INFO ] weather: applied flags 0x00 -> 0x01 flags2=0x00 rain=0.50 lc=1.00\n"
        event = "[12:00:11] [INFO ] event_fire: client REPLAY runEvent 'piramid'\n"
        report = self.paired(event + receive, send)
        self.assertEqual(report["comparisons"][0]["checked"], 2)
        self.assertEqual(report["comparisons"][0]["missing"], 0)

    def test_previous_session_apply_cannot_satisfy_new_send(self):
        report = self.paired("[12:00:09] [INFO ] event_fire: client REPLAY runEvent 'piramid'\n" +
                             "[12:00:10] [INFO ] net: session started role=client\n" +
                             "[12:00:10] [INFO ] rig: READY local-world-ready\n")
        self.assertEqual(report["comparisons"][0]["missing"], 1)

    def test_send_after_client_disconnect_is_not_a_desync(self):
        report = self.paired("[12:00:09] [INFO ] net: session ended (intentional disconnect)\n")
        self.assertEqual(report["comparisons"][0]["status"], "unproven")
        self.assertEqual(report["comparisons"][0]["missing"], 0)

    def test_memory_and_liveness_need_evidence(self):
        host = self.log("host.log", BOOT)
        client = self.log("client.log", BOOT.replace("process 11", "process 22"))
        telemetry = self.log("samples.csv", "pid,working_set_bytes,alive\n11,7000000000,true\n22,1,false\n")
        report = analyze(host, [client], telemetry)
        self.assertEqual(report["totals"]["by_category"], {"memory": 1, "liveness": 1})
        self.assertEqual(report["logs"][0]["liveness"], "alive at last sample")
        self.assertEqual(analyze(host, [client])["logs"][0]["memory_samples"], 0)

    def test_empty_log_and_midnight(self):
        host = self.log("host.log", "[23:59:59] [INFO ] tick\n[00:00:02] [INFO ] tick\n")
        client = self.log("client.log", "")
        self.assertEqual(analyze(host, [client])["totals"]["by_category"], {"liveness": 1})

    def test_missing_scenario_verdict_is_failure(self):
        host = self.log("host.log", BOOT + "[12:00:01] [INFO ] synctest_err: ARMED scenario=rejoin role=host\n")
        report = analyze(host, [self.log("client.log", BOOT)])
        self.assertEqual(report["totals"]["by_category"], {"scenario-incomplete": 1})


if __name__ == "__main__":
    unittest.main()
