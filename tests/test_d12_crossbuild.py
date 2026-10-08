"""Synthetic regression tests for legacy/current D12 aggregate-only comparison.

All names, keys, BPM values, and logs are fabricated. No owner research files.
"""
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1] / "tools" / "compare-d12-builds.py"
SPEC = importlib.util.spec_from_file_location("d12_compare_builds", TOOL)
MOD = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MOD
SPEC.loader.exec_module(MOD)


def fixture(label: str, key: int, *, build: int, modern: bool,
            seconds: int = 20, passes: int = 50, bpm: float | None = 123.0,
            source: int = 1, policy: str = "mir_hodgkinson_policy_v1",
            reason: str = "audacity_mir_full", segment_count: int = 1,
            board: bool = True) -> list[str]:
    prefix = f"foo_smart_tempo: [{label}]"
    result = [
        f"{prefix} MirAnalysisProvenance enabled=1 track_key={key} "
        f"subsong=0 build_id={build} policy_schema={policy}"
    ]
    if modern:
        result.append(
            f"{prefix} MirSamplingPlan schema=mir_sampling_plan_v1 "
            f"track_key={key} subsong=0 requested_window_seconds={seconds} "
            f"requested_passes={passes} effective_window_seconds={seconds} "
            f"effective_passes={passes} known_track_length=1 "
            f"track_length_seconds=180 offset_min_pct=20 offset_max_pct=80")
    if board:
        result.append(
            f"{prefix} MirPolicyCandidateBoard enabled=1 "
            f"schema=mir_policy_candidate_board_v1 track_key={key} index=0 "
            "origin=fullboard cluster_bpm=123.000 local_exact_bpm=123.001 "
            "local_exact_score=0.99 base_score=0.9")
    result += [
        f"{prefix} HodgkinsonPrimarySegment enabled=1 candidate={source} "
        f"segment_index=0 segment_count={segment_count} "
        f"onset_count=25 reason={reason}",
        f"{prefix} MirPolicyDecision enabled=1 source_candidate={source} "
        f"output_would_write={int(bpm is not None)} "
        f"output_would_review_hold={int(bpm is None)} output_bpm={bpm or 0}",
        f"{prefix} Decision: raw_global=n/a, final={bpm or 0}, "
        f"decision_class={'auto' if bpm is not None else 'hold'}",
        f"{prefix} Track Timing: decode_ms=100, onset_ms=100, total_ms=230",
        (f"{prefix} Final Output: {bpm} BPM, confidence=88, uncertain=0"
         if bpm is not None else
         f"{prefix} Final Output: no writable BPM "
         "(existing BPM tag left unchanged), confidence=0, uncertain=1"),
    ]
    return result


def make_log(rows: list[list[str]], *, seconds: int = 20, passes: int = 50,
             global_prefs: bool = True) -> str:
    lines = []
    if global_prefs:
        lines.append(
            "foo_smart_tempo prefs (MIR Engine): writePrecision=2, "
            f"secondsPerSample={seconds}, samplesPerSong={passes}, "
            "offsetRange=20-80%, engine=Hodgkinson/MIR")
    for row in rows:
        lines += row
    return "\n".join(lines) + "\n"


class CrossBuildTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def read(self, name: str, content: str):
        p = self.root / name
        p.write_text(content, encoding="utf-8")
        return MOD.parse(p)

    def runs(self):
        identity = 99990001
        old = fixture("Private Synthetic Artist - Private Track", identity,
                      build=123, modern=False, bpm=None, source=0,
                      board=False, reason="single_event")
        new = fixture("Private Synthetic Artist - Private Track", identity,
                      build=456, modern=True, bpm=123.0, source=1,
                      reason="audacity_mir_full")
        return (self.read("legacy.log", make_log([old])),
                self.read("new.log", make_log([new])))

    def test_legacy_to_current_comparison_is_aggregate_only(self):
        report = MOD.aggregate(*self.runs())
        self.assertEqual(report["track_count"], 1)
        self.assertTrue(report["engine_binaries_differ"])
        self.assertEqual(report["sampling_evidence"], "LEGACY_GLOBAL_UNVERIFIED")
        self.assertEqual(report["terminal_transitions"]["HOLD_TO_OUTPUT"], 1)
        self.assertEqual(report["candidate_boards_changed"], 1)
        self.assertEqual(report["baseline_segment_reasons"]["single_event"], 1)
        self.assertEqual(report["variant_segment_reasons"]["audacity_mir_full"], 1)
        encoded = json.dumps(report)
        self.assertNotIn("Private Synthetic Artist", encoded)
        self.assertNotIn("Private Track", encoded)
        self.assertNotIn("99990001", encoded)
        self.assertNotIn("123.0 BPM", encoded)

    def test_equal_builds_can_repeat_but_are_not_cross_build(self):
        old, new = self.runs()
        report = MOD.aggregate(new, new)
        self.assertFalse(report["engine_binaries_differ"])
        self.assertEqual(report["sampling_evidence"], "PER_TRACK_BOTH")
        self.assertEqual(report["candidate_boards_changed"], 0)

    def test_legacy_without_global_settings_is_rejected(self):
        lines = fixture("Synthetic", 11, build=1, modern=False)
        with self.assertRaisesRegex(MOD.EvidenceError, "global MIR settings"):
            self.read("no-prefs.log", make_log([lines], global_prefs=False))

    def test_legacy_conflicting_preferences_are_rejected(self):
        lines = fixture("Synthetic", 11, build=1, modern=False)
        data = make_log([lines])
        data += make_log([], seconds=30)
        with self.assertRaisesRegex(MOD.EvidenceError, "one unambiguous"):
            self.read("conflict.log", data)

    def test_changes_to_sampling_settings_are_rejected(self):
        old, _ = self.runs()
        new = fixture("Private Synthetic Artist - Private Track", 99990001,
                      build=456, modern=True, seconds=30)
        other = self.read("changed.log", make_log([new], seconds=30))
        with self.assertRaisesRegex(MOD.EvidenceError, "Different requested"):
            MOD.aggregate(old, other)

    def test_policy_changes_are_rejected(self):
        old, _ = self.runs()
        new = fixture("Private Synthetic Artist - Private Track", 99990001,
                      build=456, modern=True, policy="different_schema")
        other = self.read("policy.log", make_log([new]))
        with self.assertRaisesRegex(MOD.EvidenceError, "schema mismatch"):
            MOD.aggregate(old, other)

    def test_partial_modern_telemetry_rejected(self):
        a = fixture("One", 11, build=1, modern=True)
        b = fixture("Two", 22, build=1, modern=False)
        with self.assertRaisesRegex(MOD.EvidenceError, "Partially missing"):
            self.read("partial.log", make_log([a, b]))

    def test_mixed_source_builds_rejected(self):
        a = fixture("One", 11, build=1, modern=True)
        b = fixture("Two", 22, build=2, modern=True)
        with self.assertRaisesRegex(MOD.EvidenceError, "Mixed engine builds"):
            self.read("mixed.log", make_log([a, b]))

    def test_global_plan_contradiction_rejected(self):
        a = fixture("One", 11, build=1, modern=True, seconds=30)
        with self.assertRaisesRegex(MOD.EvidenceError, "contradict"):
            self.read("plan.log", make_log([a], seconds=20))

    def test_duplicate_provenance_rejected(self):
        lines = fixture("One", 11, build=1, modern=True)
        lines.append(lines[0])
        with self.assertRaisesRegex(MOD.EvidenceError, "repeated track"):
            self.read("dup.log", make_log([lines]))

    def test_missing_terminal_rejected(self):
        lines = [line for line in fixture("One", 11, build=1, modern=True)
                 if "Final Output:" not in line]
        with self.assertRaisesRegex(MOD.EvidenceError, "Incomplete run"):
            self.read("missing.log", make_log([lines]))

    def test_incomplete_segment_trace_is_explicit(self):
        old = fixture("One", 11, build=1, modern=False, segment_count=3)
        new = fixture("One", 11, build=2, modern=True, segment_count=3)
        report = MOD.aggregate(self.read("a.log", make_log([old])),
                               self.read("b.log", make_log([new])))
        self.assertEqual(report["baseline_incomplete_segment_traces"], 1)
        self.assertEqual(report["variant_incomplete_segment_traces"], 1)

    def test_duplicate_segment_identity_rejected(self):
        old = fixture("One", 11, build=1, modern=False)
        old.append(next(line for line in old if "HodgkinsonPrimarySegment" in line))
        with self.assertRaisesRegex(MOD.EvidenceError, "Duplicated"):
            self.read("duplicate-segment.log", make_log([old]))

    def test_different_tracks_fail_without_exposing_identifiers(self):
        old = fixture("One", 11, build=1, modern=False)
        new = fixture("Two", 22, build=2, modern=True)
        with self.assertRaisesRegex(MOD.EvidenceError, "different track"):
            MOD.aggregate(self.read("a.log", make_log([old])),
                          self.read("b.log", make_log([new])))

    def test_two_tracks_with_same_label_rejected(self):
        a = fixture("Same Label", 11, build=1, modern=True)
        b = fixture("Same Label", 22, build=1, modern=True)
        with self.assertRaisesRegex(MOD.EvidenceError, "Ambiguous terminal"):
            self.read("duplicate-label.log", make_log([a, b]))


if __name__ == "__main__":
    unittest.main()
