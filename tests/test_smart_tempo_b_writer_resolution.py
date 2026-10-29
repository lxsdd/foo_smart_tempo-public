"""Independent positive/negative applied-writer provenance and source contract.

All fixtures are constructed numeric decisions, never audio/music acceptance.
"""
from __future__ import annotations
import copy
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from audit_smart_tempo_b_writer_resolution import (
    WriterResolutionError, reconcile)


def row(stage, outcome, reason, bpm, parents=(), score=0, support=0):
    return dict(stage=stage, outcome=outcome, reason_code=reason,
                candidate_id=None, parent_candidate_ids=list(parents),
                metrics=dict(bpm=bpm, score=score, support=support))


def scenario(kind):
    primary = row("primary_select", "selected", "primary_auto", 128, ["c1"])
    pointer = row("policy_pointer", "observed", "pointer_not_observed", 0)
    policy = row("policy_change", "rejected", "no_policy_write", 0)
    final = row("final_output", "selected", "final_numeric", 128)
    witness = row("measured_witness", "included", "indexed_native_witness", 128)
    writer = row("writer_resolution", "selected", "mir_primary", 128,
                 ["c1"], score=1)
    if kind == "override":
        pointer = row("policy_pointer", "observed",
                      "pointer_native_fullboard_consistent", 144, ["c2"])
        policy = row("policy_change", "selected", "writer_override", 144)
        final["metrics"]["bpm"] = witness["metrics"]["bpm"] = 144
        writer = row("writer_resolution", "selected", "mir_writer_override",
                     144, ["c2"], score=1, support=1)
    elif kind == "release":
        primary = row("primary_select", "review_hold", "primary_review_hold", 0)
        pointer = row("policy_pointer", "observed",
                      "pointer_native_supplemental_consistent", 144, ["c3"])
        policy = row("policy_change", "selected", "hold_release", 144)
        final["metrics"]["bpm"] = witness["metrics"]["bpm"] = 144
        writer = row("writer_resolution", "selected", "mir_review_hold_release",
                     144, ["c3"], score=1, support=1)
    elif kind == "micro":
        pointer = row("policy_pointer", "observed",
                      "pointer_supplemental_or_changed", 128)
        policy = row("policy_change", "selected", "keep_primary", 128)
        final["metrics"]["bpm"] = witness["metrics"]["bpm"] = 128.5
        writer = row("writer_resolution", "selected", "mir_local_exact_upgrade",
                     128.5, score=1, support=1)
    elif kind in ("policy_hold", "no_candidate", "invariant"):
        final = row("final_output", "review_hold", "final_abstained", 0)
        witness = row("measured_witness", "rejected", "not_accepted", 0)
        writer = row("writer_resolution", "review_hold", "mir_review_hold", 0,
                     score=1)
        if kind == "policy_hold":
            policy = row("policy_change", "review_hold", "no_policy_write", 0)
        elif kind == "no_candidate":
            primary = row("primary_select", "no_candidate",
                          "primary_no_candidate", 0)
            writer["reason_code"] = "mir_no_candidate"
        else:
            policy = row("policy_change", "selected", "keep_primary", 128)
            writer["reason_code"] = "mir_writer_invariant_hold"
            writer["metrics"]["support"] = 1
    return [primary, pointer, policy, writer, final, witness]


class WriterResolutionTests(unittest.TestCase):
    def test_all_applied_writer_outcomes_and_unindexed_micro(self):
        expected = dict(primary=(1, 0, 0), override=(1, 0, 1),
                        release=(1, 0, 1), micro=(0, 1, 1),
                        policy_hold=(0, 0, 0), no_candidate=(0, 0, 0),
                        invariant=(0, 0, 0))
        for kind, counts in expected.items():
            with self.subTest(kind=kind):
                r = reconcile(scenario(kind), required=True)
                self.assertEqual((r["writer_source_indexed"],
                                  r["writer_source_unindexed"],
                                  r["writer_policy_applied"]), counts)
                self.assertEqual(r["writer_resolution_native"], 1)

    def test_fail_closed_native_mutations(self):
        cases = {
            "primary_wrong_source": ("primary", lambda r: r[3].update(parent_candidate_ids=["c2"])),
            "primary_wrong_bpm": ("primary", lambda r: r[3]["metrics"].update(bpm=129)),
            "primary_wrong_reason": ("primary", lambda r: r[3].update(reason_code="mir_writer_override")),
            "override_without_runtime": ("override", lambda r: r[3]["metrics"].update(score=0)),
            "override_without_policy": ("override", lambda r: r[2].update(reason_code="keep_primary")),
            "override_wrong_pointer": ("override", lambda r: r[3].update(parent_candidate_ids=["c1"])),
            "override_unindexed_forgery": ("override", lambda r: r[3].update(parent_candidate_ids=[])),
            "override_flag_spoof": ("override", lambda r: r[3]["metrics"].update(support=0)),
            "release_without_proposal": ("release", lambda r: r[2].update(reason_code="keep_primary")),
            "release_false_parent": ("release", lambda r: r[3].update(parent_candidate_ids=["c2"])),
            "micro_invented_parent": ("micro", lambda r: r[3].update(parent_candidate_ids=["c2"])),
            "micro_without_policy": ("micro", lambda r: r[2].update(outcome="rejected")),
            "held_with_parent": ("policy_hold", lambda r: r[3].update(parent_candidate_ids=["c1"])),
            "held_claims_accept": ("policy_hold", lambda r: r[3].update(outcome="selected")),
            "no_candidate_with_primary": ("no_candidate", lambda r: r[0].update(outcome="selected")),
            "invariant_forged_witness": ("invariant", lambda r: r[5].update(outcome="included")),
            "unknown_reason": ("primary", lambda r: r[3].update(reason_code="private_path")),
            "runtime_nonboolean": ("override", lambda r: r[3]["metrics"].update(score=.5)),
            "duplicate_writer": ("primary", lambda r: r.append(copy.deepcopy(r[3]))),
            "missing_writer": ("primary", lambda r: r.pop(3)),
        }
        for name, (kind, mutate) in cases.items():
            rows = scenario(kind)
            mutate(rows)
            with self.subTest(name=name), self.assertRaises(WriterResolutionError):
                reconcile(rows, required=True)

    def test_source_host_parser_and_privacy_binding(self):
        header = (ROOT / "tests/ci_pipeline_trace.h").read_text(encoding="utf-8")
        engine = (ROOT / "src/foo_smart_tempo/modern_bpm_analyzer.cpp").read_text(encoding="utf-8")
        parser = (ROOT / "tools/audit_smart_tempo_ci_pipeline_trace.py").read_text(encoding="utf-8")
        host = (ROOT / "tests/diagnostic_live_host_smoke.ps1").read_text(encoding="utf-8")
        for token in ('out.push("writer_resolution"', "applied_writer_reason",
                      "writer_reason_known", "selected_policy_pointer"):
            self.assertIn(token, header)
        self.assertIn("m_diag.policy_reason.get_ptr()", engine)
        self.assertIn("reconcile_writer_resolution(", parser)
        self.assertIn("require_policy_filter=a.require_windows", parser)
        self.assertIn("SMART_TEMPO_TRACE_WRITER_RESOLUTION_NATIVE", host)
        self.assertNotIn("trackLabel.get_ptr(),\n                measuredWitness", engine)


if __name__ == "__main__":
    unittest.main()
