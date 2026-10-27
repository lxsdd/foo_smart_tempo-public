"""Truth-blind General-144 native promotion: source-ID and ratio adversaries.

Only synthetic candidate objects. The production comparator is never tuned
against an oracle BPM or owner audio; exact host probes run separately.
"""
import copy
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))
from test_smart_tempo_ci_pipeline_trace import event, lineage_trace
from audit_smart_tempo_ci_pipeline_trace import TraceError, validate


def fixture(mode="promote"):
    if mode not in ("promote", "retain", "no_challenger", "bypass"):
        raise ValueError(mode)
    chosen = "c3" if mode == "promote" else "c2"
    bpm = 130. if chosen == "c3" else 144.
    rows = lineage_trace([144., 130.], [144., 130.], [[0], [1]])
    def make(stage, cid, reason, outcome="included", score=0.):
        x = event(stage, cid, [], 144. if cid == "c2" else 130.,
                  outcome, reason, 0)
        x["metrics"]["score"] = score
        x["metrics"]["support"] = 0.
        return x
    def pair(cid, family, score, outcome):
        rank = make("primary_rank", cid, family + "_rank_scored", score=score)
        comp = make("primary_compare", cid,
                    family + "_compare_" + outcome,
                    "included" if outcome == "won" else "excluded", score)
        if family == "general_baseline":
            comp["metrics"].update({f"tie_{i}": 0. for i in range(4)})
        return [rank, comp]
    gates = [
        make("candidate_filter", "c2", "general_in_rail"),
        make("candidate_filter", "c3", "general_in_rail"),
        *pair("c2", "general_baseline", 12., "won"),
        *pair("c3", "general_baseline", 9., "lost"),
        make("candidate_filter", "c2", "general_alt_baseline_source"),
    ]
    if mode in ("promote", "retain"):
        challenger = .75 if mode == "promote" else .55
        gates += [
            make("candidate_filter", "c2",
                 "general_alt_window_provisional_exclusion", "excluded"),
            make("candidate_filter", "c3", "general_alt_window_allowed"),
            *pair("c3", "general_alt", 5., "won"),
            make("candidate_filter", "c2", "general_alt_baseline_evidence", score=.5),
            make("candidate_filter", "c3", "general_alt_challenger_evidence",
                 score=challenger),
            make("candidate_filter", "c3",
                 "general_alt_challenger_promoted" if mode == "promote"
                 else "general_alt_challenger_rejected",
                 "included" if mode == "promote" else "excluded",
                 challenger),
        ]
    else:
        gates += [make("candidate_filter", "c2",
                       "general_alt_no_challenger" if mode == "no_challenger"
                       else "general_alt_bypass_non144")]
    for name in ("terminal_lane_choice", "terminal_residual_choice",
                 "terminal_final_choice"):
        gates.append(make("primary_terminal", chosen, name, "selected"))
    at = next(i for i, r in enumerate(rows) if r["stage"] == "primary_select")
    rows[at:at] = gates
    primary = next(r for r in rows if r["stage"] == "primary_select")
    primary["parent_candidate_ids"] = [chosen]
    primary["metrics"]["bpm"] = bpm
    policy = next(r for r in rows if r["stage"] == "policy_eligibility")
    policy["parent_candidate_ids"] = [chosen]
    policy["metrics"]["bpm"] = bpm
    for row in rows:
        if row["stage"] in ("policy_resolution", "policy_pointer",
                            "policy_change", "final_output", "measured_witness"):
            row["metrics"]["bpm"] = bpm
    for i, row in enumerate(rows):
        row["sequence"] = i
    return rows


class GeneralAlternativeCausalTests(unittest.TestCase):
    def test_four_distinct_original_selector_outcomes(self):
        for mode in ("promote", "retain", "no_challenger", "bypass"):
            with self.subTest(mode=mode):
                result = validate(fixture(mode))
                self.assertEqual(result["primary_bpm"], 130. if mode == "promote" else 144.)

    def test_independent_adversarial_source_and_promotion_mutations(self):
        for mode in ("promote", "retain", "no_challenger", "bypass"):
            for poison in ("missing_baseline", "duplicate_resolution",
                           "wrong_source", "wrong_terminal", "wrong_outcome",
                           "extra_rank"):
                with self.subTest(mode=mode, poison=poison):
                    rows = copy.deepcopy(fixture(mode))
                    base = next(i for i, r in enumerate(rows)
                                if r["reason_code"] == "general_alt_baseline_source")
                    verdict = next(i for i, r in enumerate(rows)
                                   if r["reason_code"] in (
                                       "general_alt_challenger_promoted",
                                       "general_alt_challenger_rejected",
                                       "general_alt_no_challenger",
                                       "general_alt_bypass_non144"))
                    if poison == "missing_baseline":
                        rows.pop(base)
                    elif poison == "duplicate_resolution":
                        rows.insert(verdict, copy.deepcopy(rows[verdict]))
                    elif poison == "wrong_source":
                        rows[base]["candidate_id"] = "c3"
                    elif poison == "wrong_terminal":
                        terminal = next(r for r in rows if
                                        r["reason_code"] == "terminal_lane_choice")
                        terminal["candidate_id"] = ("c2" if terminal["candidate_id"] == "c3" else "c3")
                    elif poison == "wrong_outcome":
                        rows[verdict]["outcome"] = (
                            "excluded" if rows[verdict]["outcome"] == "included"
                            else "included")
                    elif poison == "extra_rank":
                        rows.insert(verdict, make_extra_rank())
                    for i, r in enumerate(rows):
                        r["sequence"] = i
                    with self.assertRaises(TraceError):
                        validate(rows)

    def test_ratio_and_native_challenger_identity_cannot_be_forged(self):
        for mode in ("promote", "retain"):
            for poison in ("ratio", "evidence", "challenger", "missing_rank",
                           "missing_evidence", "wrong_evidence_source"):
                with self.subTest(mode=mode, poison=poison):
                    rows = copy.deepcopy(fixture(mode))
                    find = lambda reason: next(r for r in rows if r["reason_code"] == reason)
                    verdict = find("general_alt_challenger_promoted" if mode == "promote"
                                   else "general_alt_challenger_rejected")
                    if poison == "ratio":
                        find("general_alt_baseline_evidence")["metrics"]["score"] = 2.
                    elif poison == "evidence":
                        verdict["metrics"]["score"] = 10.
                    elif poison == "challenger":
                        verdict["candidate_id"] = "c2"
                    elif poison == "missing_rank":
                        rows = [r for r in rows if r["reason_code"] != "general_alt_rank_scored"]
                    elif poison == "missing_evidence":
                        rows = [r for r in rows if r["reason_code"] !=
                                "general_alt_challenger_evidence"]
                    else:
                        find("general_alt_challenger_evidence")["candidate_id"] = "c2"
                    for i, r in enumerate(rows):
                        r["sequence"] = i
                    with self.assertRaises(TraceError):
                        validate(rows)

    def test_native_source_and_host_marker_contract(self):
        src = (ROOT / "src/foo_smart_tempo/mir_candidate_pipeline.cpp").read_text()
        host = (ROOT / "tests/diagnostic_live_host_smoke.ps1").read_text()
        probe = (ROOT / "tests/smart_tempo_ci_probe.cpp").read_text()
        for name in ("general_alt_baseline_source", "general_alt_baseline_evidence",
                     "general_alt_challenger_evidence", "general_alt_challenger_promoted",
                     "general_alt_challenger_rejected", "general_alt_no_challenger",
                     "general_alt_bypass_non144"):
            self.assertIn('"' + name + '"', src)
        self.assertIn("native_general_alt_contract", probe)
        self.assertIn("SMART_TEMPO_CI_NATIVE_GENERAL_ALT=PASS", probe)
        self.assertIn(".native-general-alt", host)


def make_extra_rank():
    return event("primary_rank", "c3", [], 130., "included",
                 "general_alt_rank_scored", 0)


if __name__ == "__main__":
    unittest.main()
