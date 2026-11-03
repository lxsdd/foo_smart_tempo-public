"""Positive/negative independent nested veto ledger tests."""
from __future__ import annotations
import copy
from pathlib import Path
import sys
import unittest
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from audit_smart_tempo_b_veto_ledger import VetoLedgerError, reconcile_native_veto_ledger

def row(stage, cid, reason, outcome="observed", bpm=128., score=0., position=0):
    return {"stage": stage, "candidate_id": cid, "parent_candidate_ids": [],
            "reason_code": reason, "outcome": outcome,
            "metrics": {"bpm": bpm, "score": score, "support": position}}

def fixture():
    return [
        row("candidate_create", "c0", "raw_topk"),
        row("candidate_transform", "c1", "cluster_from_raw"),
        row("candidate_transform", "c2", "cluster_from_raw", bpm=144.),
        row("candidate_filter", "c0", "raw_alias_decisive_veto", "excluded", score=.1),
        row("candidate_filter", "c2", "low_upper_branch_veto", "excluded", bpm=144., score=.2),
        row("primary_rank", "c1", "low_rank_scored", "included"),
        row("candidate_filter", "c2", "low_continuous_branch_veto", "excluded", bpm=144., score=.3),
        row("primary_terminal", "c2", "terminal_final_choice", "selected", bpm=144.),
        row("primary_veto_ledger", "c0", "raw_alias_decisive_veto", "excluded", score=.1, position=1),
        row("primary_veto_ledger", "c2", "low_upper_branch_veto", "excluded", bpm=144., score=.2, position=2),
        row("primary_veto_ledger", "c2", "low_continuous_branch_veto", "excluded", bpm=144., score=.3, position=4),
    ]

class NativeVetoLedgerTests(unittest.TestCase):
    def test_nested_veto_and_selected_after_branch_veto(self):
        result = reconcile_native_veto_ledger(fixture(), required=True)
        self.assertEqual((result["native_veto_ledger_events"],
                          result["native_veto_ledger_raw"],
                          result["native_veto_ledger_cluster"],
                          result["native_veto_ledger_selected_after_branch_veto"]),
                         (3, 1, 2, 2))

    def test_no_veto_is_not_global_eligibility(self):
        rows = fixture()[:3] + [row("primary_terminal", "c1", "terminal_final_choice", "selected")]
        self.assertEqual(reconcile_native_veto_ledger(rows, required=True)["native_veto_ledger_events"], 0)
        self.assertEqual(reconcile_native_veto_ledger(rows)["native_veto_ledger_complete"], 0)

    def test_adversarial_mutations(self):
        for label in ("missing", "duplicate", "reorder", "source", "reason", "outcome",
                      "position", "score", "bpm", "parent", "origin",
                      "native_outcome", "native_reason", "native_score", "extra"):
            with self.subTest(label=label):
                bad = copy.deepcopy(fixture())
                if label == "missing": bad.pop()
                elif label == "duplicate": bad.append(copy.deepcopy(bad[-1]))
                elif label == "reorder": bad[-1], bad[-2] = bad[-2], bad[-1]
                elif label == "source": bad[-1]["candidate_id"] = "c1"
                elif label == "reason": bad[-1]["reason_code"] = "general_rail_decisive_veto"
                elif label == "outcome": bad[-1]["outcome"] = "observed"
                elif label == "position": bad[-1]["metrics"]["support"] = 3
                elif label == "score": bad[-1]["metrics"]["score"] = .7
                elif label == "bpm": bad[-1]["metrics"]["bpm"] = 128.
                elif label == "parent": bad[-1]["parent_candidate_ids"] = ["c0"]
                elif label == "origin": bad[2]["stage"] = "candidate_create"
                elif label == "native_outcome": bad[6]["outcome"] = "included"
                elif label == "native_reason": bad[6]["reason_code"] = "low_continuous_allowed"
                elif label == "native_score": bad[6]["metrics"]["score"] = .9
                elif label == "extra": bad.append(row("primary_veto_ledger", "c1",
                    "harmonic_rail_decisive_veto", "excluded", position=5))
                with self.assertRaises(VetoLedgerError):
                    reconcile_native_veto_ledger(bad, required=True)

    def test_cpp_parser_workflow_wiring(self):
        native = (ROOT / "tests/ci_pipeline_trace.h").read_text(encoding="utf-8")
        parser = (ROOT / "tools/audit_smart_tempo_ci_pipeline_trace.py").read_text(encoding="utf-8")
        workflow = (ROOT / ".github/workflows/source.yml").read_text(encoding="utf-8")
        self.assertIn('out.push("primary_veto_ledger"', native)
        self.assertIn("vetoLedgerCount", native)
        self.assertIn("reconcile_native_veto_ledger(", parser)
        self.assertIn("test_smart_tempo_b_veto_ledger.py", workflow)

if __name__ == "__main__":
    unittest.main()
