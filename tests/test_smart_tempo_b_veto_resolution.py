"""Independent CI-only native branch-veto to final-choice positive/negative proof."""
from __future__ import annotations
import copy
from pathlib import Path
import unittest
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from audit_smart_tempo_b_veto_resolution import (
    VetoResolutionError, reconcile_native_veto_resolution)


def row(stage, cid, reason, outcome="observed", bpm=128., score=0.):
    return dict(stage=stage, candidate_id=cid, parent_candidate_ids=[],
                reason_code=reason, outcome=outcome, sequence=0,
                metrics=dict(bpm=bpm, score=score, support=0.))


def case(selected_after_veto=False, with_veto=True):
    # c1 is selected unless residual rescue later selects the vetoed c2.
    # The veto is branch-local, not global eligibility.
    rows = [
        row("candidate_transform", "c1", "cluster_from_raw"),
        row("candidate_transform", "c2", "cluster_from_raw", bpm=230.),
    ]
    if with_veto:
        rows += [
            row("candidate_filter", "c2", "general_out_of_rail_provisional",
                "excluded", bpm=230., score=.2),
            row("candidate_filter", "c2", "general_rail_decisive_veto",
                "excluded", bpm=230., score=.2),
        ]
    rows.append(row("primary_terminal", "c2" if selected_after_veto else "c1",
                    "terminal_final_choice", "selected",
                    bpm=230. if selected_after_veto else 128.))
    rows += [
        row("primary_veto_resolution", "c1",
            "no_native_decisive_veto_observed" if selected_after_veto else
            "selected_without_native_decisive_veto",
            "observed" if selected_after_veto else "selected"),
        row("primary_veto_resolution", "c2",
            "general_rail_decisive_veto" if with_veto else
            ("selected_without_native_decisive_veto" if selected_after_veto
             else "no_native_decisive_veto_observed"),
            ("selected" if selected_after_veto else "excluded") if with_veto
            else ("selected" if selected_after_veto else "observed"),
            bpm=230., score=.2 if with_veto else 0.),
    ]
    for i, r in enumerate(rows):
        r["sequence"] = i
    if with_veto:
        # Native event index counts filter/rank/terminal, not clusters.
        rows[-1]["metrics"]["support"] = 2
    return rows


class NativeVetoResolutionTests(unittest.TestCase):
    def test_positive_branch_veto_and_final_selection_are_distinct(self):
        for selected in (False, True):
            for with_veto in (False, True):
                with self.subTest(selected=selected, veto=with_veto):
                    result = reconcile_native_veto_resolution(
                        case(selected, with_veto), required=True)
                    self.assertEqual(result["primary_veto_resolution_sources"], 2)
                    self.assertEqual(result["primary_veto_resolution_branch_vetos"],
                                     int(with_veto))
                    self.assertEqual(
                        result["primary_veto_resolution_selected_after_branch_veto"],
                        int(selected and with_veto))

    def test_adversarial_mutations_fail_closed(self):
        good = case()
        def mutate(name, rows):
            if name == "missing":
                rows.pop()
            elif name == "duplicate":
                rows.append(copy.deepcopy(rows[-1]))
            elif name == "source":
                rows[-1]["candidate_id"] = "c1"
            elif name == "reason":
                rows[-1]["reason_code"] = "harmonic_rail_decisive_veto"
            elif name == "outcome":
                rows[-1]["outcome"] = "observed"
            elif name == "index":
                rows[-1]["metrics"]["support"] = 1
            elif name == "score":
                rows[-1]["metrics"]["score"] = .4
            elif name == "bpm":
                rows[-1]["metrics"]["bpm"] = 128.
            elif name == "precursor":
                rows[2]["reason_code"] = "general_in_rail"
            elif name == "veto_forged":
                rows[3]["reason_code"] = "general_rail_fallback_readmitted"
            elif name == "readmission":
                rows.insert(4, row("candidate_filter", "c2",
                                   "general_rail_fallback_readmitted",
                                   "included", bpm=230.))
            elif name == "selected":
                rows[4]["candidate_id"] = "c2"
            elif name == "duplicate_terminal":
                rows.insert(5, copy.deepcopy(rows[4]))
            elif name == "missing_terminal":
                rows.pop(4)
            elif name == "duplicate_veto":
                rows.insert(4, copy.deepcopy(rows[3]))
            elif name == "order":
                rows[-1], rows[-2] = rows[-2], rows[-1]
            else:
                raise AssertionError(name)
            for i, item in enumerate(rows):
                item["sequence"] = i
        for name in (
            "missing", "duplicate", "source", "reason", "outcome",
            "index", "score", "bpm", "precursor", "veto_forged",
            "readmission", "selected", "duplicate_terminal",
            "missing_terminal", "duplicate_veto", "order"):
            with self.subTest(name=name):
                bad = copy.deepcopy(good)
                mutate(name, bad)
                with self.assertRaises(VetoResolutionError):
                    reconcile_native_veto_resolution(bad, required=True)

    def test_empty_and_legacy_nonhost_traces(self):
        self.assertEqual(reconcile_native_veto_resolution([])["primary_veto_resolution_sources"], 0)
        self.assertEqual(reconcile_native_veto_resolution([], required=True)["primary_veto_resolution_sources"], 0)
        with self.assertRaises(VetoResolutionError):
            reconcile_native_veto_resolution(case()[:-2], required=True)

    def test_native_source_parser_and_host_gate_contract(self):
        source = (ROOT / "tests" / "ci_pipeline_trace.h").read_text(encoding="utf-8")
        parser = (ROOT / "tools" / "audit_smart_tempo_ci_pipeline_trace.py").read_text(encoding="utf-8")
        workflow = (ROOT / ".github" / "workflows" / "source.yml").read_text(encoding="utf-8")
        self.assertIn('out.push("primary_veto_resolution"', source)
        self.assertIn('std::strcmp(event.reason, "general_rail_decisive_veto")', source)
        self.assertIn("firstVeto != nullptr ? firstVeto->score", source)
        self.assertIn("board.raw_candidates.size() + 5 * collapsed.size()", source)
        self.assertIn("reconcile_native_veto_resolution(", parser)
        self.assertIn("required=require_windows", parser)
        self.assertIn("test_smart_tempo_b_veto_resolution.py", workflow)
        self.assertNotRegex(workflow, r"(?m)^\s*uses:\s*actions/upload-artifact@")
        self.assertNotIn("metadb_io::get()->update_info", source)


if __name__ == "__main__":
    unittest.main()
