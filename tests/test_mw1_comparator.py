"""Synthetic regression tests for the public, reference-blind MW1 comparator."""
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "compare-mir-window-runs.py"
SPEC = importlib.util.spec_from_file_location("mir_window_compare", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def candidate(track: str, key: int, bpm: int = 120, score: float = 0.2) -> str:
    values = {name: "1" for name in MODULE.CANDIDATE_FIELDS}
    values.update(cluster_bpm=str(bpm), local_exact_bpm=str(bpm),
                  local_exact_score=str(score), base_score="0.75",
                  alias_classes="direct,half")
    fields = " ".join(name + "=" + value for name, value in values.items())
    return (f"foo_smart_tempo: [{track}] MirPolicyCandidateBoard "
            f"enabled=1 schema=mir_policy_candidate_board_v1 "
            f"track_key={key} index=0 origin=fullboard {fields}")


def track_lines(label: str, key: int, *, seconds: int = 20,
                passes: int = 50, bpm: int | None = 120,
                candidate_bpm: int | None = 120, build: str = "123",
                subsong: int = 0):
    pre = f"foo_smart_tempo: [{label}]"
    lines = [
        f"{pre} MirAnalysisProvenance enabled=1 track_key={key} "
        f"subsong={subsong} build_id={build} policy_schema=mir_policy_test_v1",
        f"{pre} MirSamplingPlan schema=mir_sampling_plan_v1 track_key={key} "
        f"subsong={subsong} requested_window_seconds={seconds} "
        f"requested_passes={passes} effective_window_seconds={seconds} "
        f"effective_passes={passes} known_track_length=1 "
        "track_length_seconds=240 offset_min_pct=20 offset_max_pct=80",
    ]
    if candidate_bpm is not None:
        lines.append(candidate(label, key, candidate_bpm))
    status = "1" if bpm else "0"
    lines.extend([
        f"{pre} MirPolicyDecision enabled=1 output_would_write={status} "
        f"output_would_review_hold={int(not bool(bpm))} output_bpm={bpm or 0}",
        f"{pre} Decision: raw_global=n/a, raw_projected=n/a, "
        f"final={bpm or 0}.00, decision_class={'auto' if bpm else 'hold'}",
        f"{pre} Track Timing: decode_ms=100, onset_ms=200, total_ms=500",
        f"{pre} Final Output: {str(bpm) + '.00 BPM' if bpm else 'no writable BPM (existing BPM tag left unchanged)'}, "
        f"confidence=91.0, uncertain={int(not bool(bpm))}, decision_class={'auto' if bpm else 'hold'}",
    ])
    return lines


class EvidenceComparatorTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def log(self, lines: list[str], name: str) -> Path:
        p = self.root / name
        p.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return p

    def test_compare_interleaved_tracks_and_anonymize(self):
        a = track_lines("Synthetic One", 11) + track_lines("Synthetic Two", 22, bpm=None, candidate_bpm=None)
        b = track_lines("Synthetic One", 11, seconds=10, candidate_bpm=121) + track_lines("Synthetic Two", 22, seconds=10, bpm=130)
        ta = MODULE.parse_log(self.log(a, "base.txt"))
        tb = MODULE.parse_log(self.log(b, "variant.txt"))
        result = MODULE.compare(ta, tb)
        self.assertEqual(result["track_count"], 2)
        self.assertEqual(result["hold_to_write"], 1)
        self.assertEqual(result["write_to_write"], 1)
        self.assertEqual(result["candidate_boards_changed"], 2)
        self.assertEqual([row["track"] for row in result["rows"]], ["T0001", "T0002"])
        output = json.dumps(result)
        self.assertNotIn("Synthetic One", output)
        self.assertNotIn("Synthetic Two", output)
        self.assertNotIn('"11"', output)
        self.assertNotIn('"22"', output)

    def test_locale_comma_decimal_final(self):
        lines = track_lines("Synthetic Three", 31)
        lines[-1] = lines[-1].replace("120.00 BPM", "120,00 BPM")
        tracks = MODULE.parse_log(self.log(lines, "comma.txt"))
        self.assertEqual(next(iter(tracks.values())).final["bpm"], 120.0)

    def test_duplicate_provenance_rejected(self):
        lines = track_lines("Synthetic One", 11)
        lines.append(lines[0])
        with self.assertRaisesRegex(MODULE.EvidenceError, "Duplicate track provenance"):
            MODULE.parse_log(self.log(lines, "duplicate.txt"))

    def test_missing_per_track_sampling_plan_rejected(self):
        lines = [x for x in track_lines("Synthetic One", 11) if "MirSamplingPlan" not in x]
        with self.assertRaisesRegex(MODULE.EvidenceError, "Incomplete"):
            MODULE.parse_log(self.log(lines, "missing.txt"))

    def test_label_collision_fails_closed(self):
        lines = track_lines("Same Label", 11) + track_lines("Same Label", 22)
        with self.assertRaisesRegex(MODULE.EvidenceError, "Ambiguous or missing label-only"):
            MODULE.parse_log(self.log(lines, "collision.txt"))

    def test_mismatched_build_rejected(self):
        a = MODULE.parse_log(self.log(track_lines("Synthetic One", 11, build="1"), "a.txt"))
        b = MODULE.parse_log(self.log(track_lines("Synthetic One", 11, build="2"), "b.txt"))
        with self.assertRaisesRegex(MODULE.EvidenceError, "Build or policy schema"):
            MODULE.compare(a, b)

    def test_mismatched_track_set_rejected(self):
        a = MODULE.parse_log(self.log(track_lines("Synthetic One", 11), "a.txt"))
        b = MODULE.parse_log(self.log(track_lines("Synthetic Two", 22), "b.txt"))
        with self.assertRaisesRegex(MODULE.EvidenceError, "Track identity mismatch"):
            MODULE.compare(a, b)

    def test_ambiguous_duplicate_terminal_rejected(self):
        lines = track_lines("Synthetic One", 11)
        lines.append(next(line for line in lines if "Final Output:" in line))
        with self.assertRaisesRegex(MODULE.EvidenceError, "Duplicate final output"):
            MODULE.parse_log(self.log(lines, "double.txt"))

    def test_unsupported_plan_schema_rejected(self):
        lines = [l.replace("schema=mir_sampling_plan_v1", "schema=other")
                 for l in track_lines("Synthetic One", 11)]
        with self.assertRaisesRegex(MODULE.EvidenceError, "Unsupported sampling plan schema"):
            MODULE.parse_log(self.log(lines, "schema.txt"))

    def test_candidate_board_full_signature_avoids_bpm_only_false_parity(self):
        a = MODULE.parse_log(self.log(track_lines("Synthetic One", 11), "a.txt"))
        b_lines = track_lines("Synthetic One", 11)
        b_lines = [l.replace("local_exact_score=0.2", "local_exact_score=0.3") for l in b_lines]
        b = MODULE.parse_log(self.log(b_lines, "b.txt"))
        result = MODULE.compare(a, b)
        self.assertEqual(result["candidate_boards_changed"], 1)

    def test_final_bpm_without_measured_decision_rejected(self):
        lines = [l.replace("final=120.00", "final=0.00") for l in track_lines("Synthetic One", 11)]
        with self.assertRaisesRegex(MODULE.EvidenceError, "contradicts"):
            MODULE.parse_log(self.log(lines, "contradiction.txt"))


if __name__ == "__main__":
    unittest.main()
