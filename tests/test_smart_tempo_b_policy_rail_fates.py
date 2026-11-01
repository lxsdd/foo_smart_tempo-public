"""Independent CI-only native Policy FullBoard rail source-index proof."""
import copy
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from audit_smart_tempo_ci_pipeline_trace import TraceError, validate
from test_smart_tempo_ci_pipeline_trace import event, lineage_trace, empty_board_native_trace


def fixture():
    # Same source-ID pool: one FullBoard survivor and one original rail veto.
    rows = lineage_trace([128., 230.], [128., 230.], [[0], [1]])
    at = next(i for i, row in enumerate(rows) if row["stage"] == "policy_eligibility")
    allowed = event("policy_filter", "c2", [], 128., "included",
                    "policy_rail_allowed", 0)
    veto = event("policy_filter", "c3", [], 230., "excluded",
                 "policy_rail_decisive_veto", 0)
    allowed["metrics"] = dict(bpm=128., score=127.99, support=0.)
    veto["metrics"] = dict(bpm=230., score=230., support=0.)
    rows[at:at] = [allowed, veto]
    for i, row in enumerate(rows):
        row["sequence"] = i
    return rows


class NativePolicyRailTests(unittest.TestCase):
    def test_native_source_and_fullboard_conservation(self):
        result = validate(fixture(), require_policy_filter=True)
        self.assertEqual((result["policy_rail_source_observations"],
                          result["policy_rail_decisive_vetos"],
                          result["policy_rail_native_complete"]), (2, 1, 1))
        self.assertEqual(result["policy_board_unique_cluster_sources"], 1)

    def test_legacy_uninstrumented_trace_cannot_claim_native_proof(self):
        old = lineage_trace([128.], [128.], [[0]])
        self.assertEqual(validate(old)["policy_rail_native_complete"], 0)
        with self.assertRaises(TraceError):
            validate(old, require_policy_filter=True)
        self.assertEqual(validate(empty_board_native_trace(),
                                  require_policy_filter=True)["raw"], 0)

    def test_adversarial_rail_veto_identity_and_exact_bpm(self):
        for name in ("missing_one", "missing_all", "duplicate", "swap",
                     "orphan", "wrong_source_bpm", "wrong_resolved_bpm",
                     "forged_veto", "forged_inclusion", "unknown_reason",
                     "nan", "wrong_support", "unexpected_parent",
                     "extra_metric", "wrong_board_parent", "wrong_board_bpm"):
            with self.subTest(name=name):
                rows = copy.deepcopy(fixture())
                filters = [r for r in rows if r["stage"] == "policy_filter"]
                member = next(r for r in rows if r["stage"] == "policy_eligibility")
                if name == "missing_one":
                    rows.remove(filters[1])
                elif name == "missing_all":
                    rows = [r for r in rows if r["stage"] != "policy_filter"]
                elif name == "duplicate":
                    rows.insert(rows.index(filters[1]), copy.deepcopy(filters[0]))
                elif name == "swap":
                    filters[0]["candidate_id"], filters[1]["candidate_id"] = (
                        filters[1]["candidate_id"], filters[0]["candidate_id"])
                elif name == "orphan":
                    filters[0]["candidate_id"] = "c999999"
                elif name == "wrong_source_bpm":
                    filters[0]["metrics"]["bpm"] = 130.
                elif name == "wrong_resolved_bpm":
                    filters[0]["metrics"]["score"] = 128.5
                elif name == "forged_veto":
                    filters[0]["outcome"] = "excluded"
                    filters[0]["reason_code"] = "policy_rail_decisive_veto"
                elif name == "forged_inclusion":
                    filters[1]["outcome"] = "included"
                    filters[1]["reason_code"] = "policy_rail_allowed"
                elif name == "unknown_reason":
                    filters[0]["reason_code"] = "invented_rail"
                elif name == "nan":
                    filters[0]["metrics"]["score"] = float("nan")
                elif name == "wrong_support":
                    filters[0]["metrics"]["support"] = 1.
                elif name == "unexpected_parent":
                    filters[0]["parent_candidate_ids"] = ["c0"]
                elif name == "extra_metric":
                    filters[0]["metrics"]["track_path"] = 1.
                elif name == "wrong_board_parent":
                    member["parent_candidate_ids"] = ["c3"]
                else:
                    member["metrics"]["bpm"] = 128.4
                for i, row in enumerate(rows):
                    row["sequence"] = i
                with self.assertRaises(TraceError):
                    validate(rows, require_policy_filter=True)

    def test_actual_cpp_and_host_contract_wiring(self):
        cpp = (ROOT / "src/foo_smart_tempo/mir_candidate_pipeline.cpp").read_text(
            encoding="utf-8")
        reporter = (ROOT / "tests/ci_pipeline_trace.h").read_text(encoding="utf-8")
        workflow = (ROOT / ".github/workflows/source.yml").read_text(encoding="utf-8")
        parser = (ROOT / "tools/audit_smart_tempo_ci_pipeline_trace.py").read_text(
            encoding="utf-8")
        self.assertIn("ci_policy_rail_event(sourceIndex, item.candidate.bpm, bpm, inRail)", cpp)
        self.assertIn("mir_ci_policy_rail_trace_complete()", reporter)
        self.assertIn('out.push("policy_filter"', reporter)
        self.assertIn("require_policy_filter=a.require_windows", parser)
        self.assertIn("test_smart_tempo_b_policy_rail_fates.py", workflow)


if __name__ == "__main__":
    unittest.main()
