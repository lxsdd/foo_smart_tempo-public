"""CI-only native BPM-pipeline lineage observer: strict positive/negative tests."""
from __future__ import annotations
import copy
import json
import math
import re
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from audit_smart_tempo_ci_pipeline_trace import TraceError, load, validate, SCHEMA, ENUMS


def event(stage, cid, parents, bpm, outcome, reason, number):
    return {
        "schema": SCHEMA, "analysis_id": "cafefeed01234567",
        "sequence": number, "stage": stage, "outcome": outcome,
        "reason_code": reason, "candidate_id": cid,
        "parent_candidate_ids": parents,
        "metrics": {"bpm": bpm, "score": .95, "support": .85}
    }


def trace(held=False):
    rows = [
        event("candidate_create", "c0", [], 128., "observed", "raw_alias", 0),
        event("candidate_transform", "c1", ["c0"], 128., "observed", "cluster_from_raw", 1),
        event("primary_eligibility", "c1", [], 128., "observed", "pool_only_gate_unresolved", 2),
        event("policy_eligibility", "c2", ["c1"], 127.99, "included", "full_board_member", 3),
        event("primary_select", None, ["c1"], 128., "selected", "primary_auto", 4),
        event("policy_resolution", None, ["c2"], 127.99, "observed",
              "policy_one_same_bpm", 5),
        event("policy_pointer", None, [], 127.99, "observed",
              "pointer_not_observed", 6),
        event("policy_change", None, [], 127.99, "selected", "keep_primary", 7),
        event("final_output", None, [], 0. if held else 127.99,
              "review_hold" if held else "selected",
              "final_abstained" if held else "final_numeric", 8),
        event("measured_witness", None, [] if held else ["c2"],
              0. if held else 127.99,
              "rejected" if held else "included",
              "not_accepted" if held else "indexed_native_witness", 9),
        event("write_guard", None, [], 0., "write_blocked", "ci_dry_run", 10)
    ]
    rows[5]["metrics"]["score"] = 0.
    rows[5]["metrics"]["support"] = 0.
    rows[6]["metrics"]["score"] = 0.
    rows[6]["metrics"]["support"] = 0.
    # The real CI recorder emits a zero-valued write_guard; do not let the
    # generic event helper's placeholder score/support pollute positive fixtures.
    rows[-1]["metrics"]["score"] = 0.
    rows[-1]["metrics"]["support"] = 0.
    return rows


def resource_event():
    item = event("resource_usage", None, [], 0., "observed",
                 "process_resource_snapshot", 0)
    item["metrics"] = {
        "bpm": 0., "score": 0., "support": 0.,
        "wall_seconds": 4.5, "process_cpu_seconds": 3.75,
        "process_peak_rss_before_bytes": 35_000_000,
        "process_peak_rss_after_bytes": 46_000_000,
        "process_working_set_after_bytes": 42_000_000
    }
    return item


def with_resource(rows):
    # Host-valid minimal selector lineage: the native recorder emits these
    # three ordered snapshots before primary_select for a chosen BPM.
    primary_index = next(i for i, row in enumerate(rows)
                         if row["stage"] == "primary_select")
    primary = rows[primary_index]
    if primary["metrics"]["bpm"] > 0:
        for reason in ("terminal_lane_choice", "terminal_residual_choice",
                       "terminal_final_choice"):
            rows.insert(primary_index, event(
                "primary_terminal", primary["parent_candidate_ids"][0], [],
                primary["metrics"]["bpm"], "selected", reason, 0))
            primary_index += 1
    # The native recorder now emits a disposition for each actual
    # collapsed candidate. This helper models the one-candidate positive
    # synthetic host fixture, not the production scoring path.
    selected_cid = primary["parent_candidate_ids"][0]
    for i, row in enumerate(rows):
        if row["stage"] == "primary_select":
            rows.insert(i, event("primary_disposition", selected_cid, [],
                                 primary["metrics"]["bpm"], "observed",
                                 "selected_final_object", 0))
            rows[i]["metrics"]["score"] = 0.
            rows[i]["metrics"]["support"] = 0.
            break
    rows.insert(-1, resource_event())
    for i, row in enumerate(rows):
        row["sequence"] = i
    return rows


class NativePipelineTraceTests(unittest.TestCase):
    def test_positive_exact_lineage_and_unknown_gate_reported_not_hidden(self):
        result = validate(trace())
        self.assertEqual((result["raw"], result["collapsed"], result["primary_pool"],
                          result["policy_board"]), (1, 1, 1, 1))
        self.assertEqual((result["primary_auto"], result["final_accept"],
                          result["write_guard_blocked"]), (1, 1, 1))
        self.assertEqual(result["primary_veto_reason_unknown"], 1)
        self.assertEqual(result["phase_b_causal_completeness"], "INCOMPLETE_PRIMARY_VETO")

    def test_primary_outcome_reason_pairs_are_not_interchangeable(self):
        for outcome, reason in (("selected", "primary_review_hold"),
                                ("review_hold", "primary_auto"),
                                ("no_candidate", "primary_auto"),
                                ("rejected", "primary_auto")):
            with self.subTest(outcome=outcome, reason=reason):
                rows = trace()
                rows[4]["outcome"] = outcome
                rows[4]["reason_code"] = reason
                with self.assertRaises(TraceError):
                    validate(rows)
        rows = trace()
        rows[4]["metrics"]["bpm"] = 0.
        with self.assertRaises(TraceError):
            validate(rows)

    def test_policy_flags_and_action_must_match_both_directions(self):
        # Native reason precedence: override > hold release > keep > no write.
        for override, release, outcome, reason, should_pass in (
            (0, 0, "selected", "keep_primary", True),
            (1, 0, "selected", "writer_override", True),
            (0, 1, "selected", "hold_release", True),
            (1, 1, "selected", "writer_override", True),
            (0, 0, "selected", "writer_override", False),
            (0, 0, "selected", "hold_release", False),
            (1, 0, "selected", "keep_primary", False),
            (0, 1, "selected", "keep_primary", False),
            (1, 1, "selected", "hold_release", False),
            (0, 0, "selected", "no_policy_write", False),
            (0, 0, "review_hold", "no_policy_write", True),
        ):
            with self.subTest(override=override, release=release,
                              outcome=outcome, reason=reason):
                rows = trace()
                rows[5]["metrics"]["score"] = float(override)
                rows[5]["metrics"]["support"] = float(release)
                rows[7]["outcome"] = outcome
                rows[7]["reason_code"] = reason
                if should_pass:
                    self.assertEqual(validate(rows)["policy_override_active"],
                                     override)
                else:
                    with self.assertRaises(TraceError):
                        validate(rows)

    def test_write_guard_rejects_forged_numeric_evidence(self):
        for metric in ("bpm", "score", "support"):
            with self.subTest(metric=metric):
                rows = trace()
                rows[-1]["metrics"][metric] = 1.
                with self.assertRaises(TraceError):
                    validate(rows)

    def test_host_primary_requires_executed_terminal_chain(self):
        rows = with_resource(trace())
        window = event("decode_window", None, [], 0., "observed",
                       "actual_decoded_extent", 0)
        window["metrics"] = dict(
            bpm=0., score=0., support=0., pass_index=0, pass_total=1,
            start_sec=0., requested_frames=32000, decoded_frames=32000,
            analysis_frames=32000, source_rate_hz=16000, analysis_rate_hz=16000)
        rows.insert(0, window)
        for i, row in enumerate(rows):
            row["sequence"] = i
        self.assertEqual(validate(rows, require_windows=True)["selector_terminal_observations"], 3)
        for missing in ("terminal_lane_choice", "terminal_residual_choice",
                        "terminal_final_choice"):
            with self.subTest(missing=missing):
                bad = copy.deepcopy(rows)
                bad = [row for row in bad if row["reason_code"] != missing]
                for i, row in enumerate(bad):
                    row["sequence"] = i
                with self.assertRaises(TraceError):
                    validate(bad, require_windows=True)
        no_terminal = [row for row in rows if row["stage"] not in
                       ("primary_terminal", "primary_disposition")]
        for i, row in enumerate(no_terminal):
            row["sequence"] = i
        self.assertEqual(validate(no_terminal)["selector_terminal_observations"], 0)
        with self.assertRaises(TraceError):
            validate(no_terminal, require_windows=True)

    def test_held_result_preserves_no_write_semantics(self):
        actual = trace(held=True)
        self.assertEqual(validate(actual)["final_accept"], 0)
        actual[8]["outcome"] = "selected"
        with self.assertRaises(TraceError):
            validate(actual)

    def test_missing_duplicate_and_reordered_terminal_stages_rejected(self):
        for edit in ("missing", "duplicate", "swap"):
            with self.subTest(edit=edit):
                actual = trace()
                if edit == "missing":
                    actual.pop()
                elif edit == "duplicate":
                    actual.insert(9, copy.deepcopy(actual[8]))
                else:
                    actual[8], actual[9] = actual[9], actual[8]
                with self.assertRaises(TraceError):
                    validate(actual)

    def test_foreign_or_orphan_candidate_ids_fail_closed(self):
        for index, key, wrong in (
            (1, "parent_candidate_ids", ["c99"]),
            (4, "parent_candidate_ids", ["c0"]),
            (2, "candidate_id", "c0"),
            (3, "candidate_id", "c1"),
            (0, "candidate_id", "c4"),
        ):
            with self.subTest(index=index,key=key):
                actual = trace()
                actual[index][key] = wrong
                with self.assertRaises(TraceError):
                    validate(actual)

    def test_unsupported_origin_private_data_or_relabeling_rejected(self):
        for index, field, changed in (
            (0, "title", "private-track"),
            (2, "reason_code", "primary_gate_passed"),
            (3, "reason_code", "tag_from_owner"),
            (4, "stage", "metadata_source"),
            (8, "reason_code", "oracle_label"),
            (10, "reason_code", "write_permitted"),
        ):
            with self.subTest(field=field):
                actual = trace()
                actual[index][field] = changed
                with self.assertRaises(TraceError):
                    validate(actual)

    def test_numeric_nan_bool_and_out_of_rail_rejected(self):
        for value in (float("nan"), float("inf"), -2., 2001., "128", True):
            with self.subTest(value=str(value)):
                actual = trace()
                actual[0]["metrics"]["bpm"] = value
                with self.assertRaises(TraceError):
                    validate(actual)
        actual = trace()
        actual[0]["metrics"]["confidence_percent"] = 98.
        with self.assertRaises(TraceError):
            validate(actual)

    def test_numeric_policy_class_and_supplement_must_not_invent_witness(self):
        actual = trace()
        actual[7]["reason_code"] = "writer_override"
        actual[5]["metrics"]["score"] = 1.
        self.assertEqual(validate(actual)["policy_board"], 1)
        actual[3]["parent_candidate_ids"] = []
        actual[3]["reason_code"] = "full_board_origin_unknown"
        r = validate(actual)
        self.assertEqual(r["policy_origin_unknown"], 1)
        self.assertEqual(r["primary_veto_reason_unknown"], 1)

    def test_executed_selector_gate_events_are_joined_to_raw_or_cluster(self):
        events = trace()
        gates = [
            event("candidate_filter", "c0", [], 128., "included",
                  "raw_alias_allowed", 0),
            event("candidate_filter", "c0", [], 128., "excluded",
                  "raw_support_provisional_exclusion", 0),
            event("primary_rank", "c0", [], 128., "included",
                  "raw_rank_scored", 0),
            event("candidate_filter", "c1", [], 128., "included",
                  "general_in_rail", 0),
            event("primary_rank", "c1", [], 128., "included",
                  "harmonic_rank_scored", 0),
        ]
        events[4:4] = gates
        for i, row in enumerate(events):
            row["sequence"] = i
        outcome = validate(events)
        self.assertEqual((outcome["selector_branch_checks"],
                          outcome["selector_rank_observations"]), (3, 2))
        self.assertEqual(outcome["primary_veto_reason_unknown"], 1)
        for broken in ("wrong_source", "unknown_reason", "invalid_outcome"):
            with self.subTest(broken=broken):
                rows = copy.deepcopy(events)
                if broken == "wrong_source":
                    rows[4]["candidate_id"] = "c1"
                if broken == "unknown_reason":
                    rows[4]["reason_code"] = "unvalidated_mir_rescue"
                if broken == "invalid_outcome":
                    rows[4]["outcome"] = "review_hold"
                with self.assertRaises(TraceError):
                    validate(rows)

    def test_executed_fallback_readmission_requires_prior_exclusion(self):
        rows = trace()
        first = event("candidate_filter", "c1", [], 128., "excluded",
                      "low_threshold_provisional_exclusion", 0)
        second = event("candidate_filter", "c1", [], 128., "included",
                       "low_threshold_fallback_readmitted", 0)
        rescue = event("candidate_filter", "c1", [], 128., "included",
                       "residual_high_gate_allowed", 0)
        rank = event("primary_rank", "c1", [], 128., "included",
                     "low_rank_scored", 0)
        rows[4:4] = [first, second, rescue, rank]
        for i, row in enumerate(rows):
            row["sequence"] = i
        report = validate(rows)
        self.assertEqual(report["selector_fallback_readmissions"], 1)
        self.assertEqual(report["selector_nested_rescue_checks"], 1)
        for kind in ("missing_exclusion", "wrong_order", "wrong_readmission_outcome",
                     "foreign_readmission_candidate", "unknown_rescue"):
            with self.subTest(kind=kind):
                broken = copy.deepcopy(rows)
                if kind == "missing_exclusion":
                    broken.pop(4)
                elif kind == "wrong_order":
                    broken[4], broken[5] = broken[5], broken[4]
                elif kind == "wrong_readmission_outcome":
                    broken[5]["outcome"] = "excluded"
                elif kind == "foreign_readmission_candidate":
                    broken[5]["candidate_id"] = "c0"
                else:
                    broken[6]["reason_code"] = "residual_unverified_guess"
                for i, row in enumerate(broken):
                    row["sequence"] = i
                with self.assertRaises(TraceError):
                    validate(broken)

    def test_actual_lane_residual_final_terminal_chain(self):
        rows = trace()
        terminal = [
            event("primary_terminal", "c1", [], 128., "selected",
                  "terminal_lane_choice", 0),
            event("primary_terminal", "c1", [], 128., "selected",
                  "terminal_residual_choice", 0),
            event("primary_terminal", "c1", [], 128., "selected",
                  "terminal_final_choice", 0),
        ]
        rows[4:4] = terminal
        for i, row in enumerate(rows):
            row["sequence"] = i
        self.assertEqual(validate(rows)["selector_terminal_observations"], 3)
        for corruption in ("missing", "reordered", "wrong_identity"):
            with self.subTest(corruption=corruption):
                bad = copy.deepcopy(rows)
                if corruption == "missing":
                    bad.pop(5)
                elif corruption == "reordered":
                    bad[4], bad[5] = bad[5], bad[4]
                else:
                    bad[4]["candidate_id"] = "c0"
                with self.assertRaises(TraceError):
                    validate(bad)

    def test_primary_terminal_winner_must_be_exact_object_not_equal_bpm(self):
        rows = trace()
        # Two collapsed candidates can share identical BPM; the actual native
        # final pointer, not a match on bpm text, chooses the winner.
        # Keep staged creator IDs and terminal/event ordering internally sound.
        # A simple falsification is to assign an unrelated primary identity
        # while retaining the exactly equal BPM and valid known object.
        terminal = [
            event("primary_terminal", "c1", [], 128., "selected",
                  "terminal_lane_choice", 0),
            event("primary_terminal", "c1", [], 128., "selected",
                  "terminal_residual_choice", 0),
            event("primary_terminal", "c1", [], 128., "selected",
                  "terminal_final_choice", 0)
        ]
        rows[4:4] = terminal
        for i, row in enumerate(rows):
            row["sequence"] = i
        self.assertEqual(validate(rows)["selector_terminal_observations"], 3)
        rows[7]["parent_candidate_ids"] = ["c2"]  # same ~BPM, policy object != primary object
        with self.assertRaisesRegex(TraceError, "matching clustered witness|exact terminal object"):
            validate(rows)

    def test_raw_aliased_candidates_can_exceed_output_rail_but_not_safety_limit(self):
        rows = trace()
        rows[0]["metrics"]["bpm"] = 512.0
        # Future lineage validator should also reject this broken parent.
        with self.assertRaises(TraceError):
            validate(rows)
        rows[1]["metrics"]["bpm"] = 512.0
        rows[2]["metrics"]["bpm"] = 512.0
        rows[4]["metrics"]["bpm"] = 512.0
        rows[3]["parent_candidate_ids"] = []
        rows[3]["reason_code"] = "full_board_origin_unknown"
        result = validate(rows)
        self.assertEqual(result["raw"], 1)

    def test_strict_file_framing_limits_and_source_id(self):
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp) / "pipeline.jsonl"
            p.write_bytes("".join(json.dumps(x) + "\n" for x in trace()).encode("utf-8"))
            self.assertEqual(validate(load(p))["rows"], 11)
            raw = p.read_text(encoding="utf-8")
            p.write_bytes(raw.rstrip("\n").encode("utf-8"))
            with self.assertRaises(TraceError):
                load(p)
            p.write_bytes(raw.replace(SCHEMA, "private_report", 1).encode("utf-8"))
            with self.assertRaises(TraceError):
                validate(load(p))
            p.write_bytes(raw.replace('"schema": ', '"music_path": "owner", "schema": ', 1).encode("utf-8"))
            with self.assertRaises(TraceError):
                validate(load(p))

    def test_final_bpm_witness_falsification_and_unindexed_distinction(self):
        rows = trace()
        self.assertEqual(validate(rows)["indexed_native_final_witness"], 1)
        for mutation in ("missing", "unrelated_value", "orphan", "fake_indexed",
                         "fake_unindexed", "wrong_outcome"):
            with self.subTest(mutation=mutation):
                modified = copy.deepcopy(rows)
                if mutation == "missing":
                    modified.pop(9)
                elif mutation == "unrelated_value":
                    modified[8]["metrics"]["bpm"] = 167.9
                elif mutation == "orphan":
                    modified[9]["parent_candidate_ids"] = ["c99"]
                elif mutation == "fake_indexed":
                    modified[9]["parent_candidate_ids"] = ["c0"]
                elif mutation == "fake_unindexed":
                    modified[9]["reason_code"] = "supplemental_witness_unindexed"
                else:
                    modified[9]["outcome"] = "rejected"
                with self.assertRaises(TraceError):
                    validate(modified)
        # A genuine supplemental candidate is not assigned a fake FullBoard
        # candidate ID: preserve explicit incomplete causal attribution.
        rows[9]["parent_candidate_ids"] = []
        rows[9]["reason_code"] = "supplemental_witness_unindexed"
        result = validate(rows)
        self.assertEqual(result["indexed_native_final_witness"], 0)
        self.assertEqual(result["unindexed_supplemental_final_witness"], 1)

    def test_run_local_id_must_match_every_event_and_have_hex_shape(self):
        rows = trace()
        for row in rows:
            row["analysis_id"] = "19a2b3c4d5e6f708"
        self.assertEqual(validate(rows)["raw"], 1)
        rows[4]["analysis_id"] = "cafefeed01234567"
        with self.assertRaises(TraceError):
            validate(rows)
        rows[4]["analysis_id"] = "19a2b3c4d5e6f708"
        rows[0]["analysis_id"] = "REUSED_CONSTANT"
        with self.assertRaises(TraceError):
            validate(rows)


    def test_native_comparator_chronology_positive_and_adversarial(self):
        base = with_resource(trace())
        insert_at = next(i for i, row in enumerate(base)
                         if row["stage"] == "primary_disposition")
        rank = event("primary_rank", "c1", [], 128., "included",
                     "general_baseline_rank_scored", 0)
        comparison = event("primary_compare", "c1", [], 128., "included",
                           "general_baseline_compare_won", 0)
        base[insert_at:insert_at] = [rank, comparison]
        for i, row in enumerate(base):
            row["sequence"] = i
        result = validate(base)
        self.assertEqual(result["selector_comparator_observations"], 1)
        self.assertEqual(result["selector_comparator_families"], 1)
        for kind in ("orphan", "wrong_id", "wrong_score", "wrong_family",
                     "first_loses", "bad_outcome", "unapproved_reason"):
            with self.subTest(kind=kind):
                bad = copy.deepcopy(base)
                comp = next(row for row in bad if row["stage"] == "primary_compare")
                if kind == "orphan":
                    bad = [row for row in bad if row["stage"] != "primary_rank"]
                elif kind == "wrong_id":
                    comp["candidate_id"] = "c0"
                elif kind == "wrong_score":
                    comp["metrics"]["score"] += .01
                elif kind == "wrong_family":
                    comp["reason_code"] = "harmonic_compare_won"
                elif kind == "first_loses":
                    comp["reason_code"] = "general_baseline_compare_lost"
                    comp["outcome"] = "excluded"
                elif kind == "bad_outcome":
                    comp["outcome"] = "excluded"
                else:
                    comp["reason_code"] = "fabricated_compare_won"
                for i, row in enumerate(bad):
                    row["sequence"] = i
                with self.assertRaises(TraceError):
                    validate(bad)

    def test_native_comparator_source_is_compile_only_and_all_four_branches_instrumented(self):
        source = (ROOT / "src/foo_smart_tempo/mir_candidate_pipeline.cpp").read_text(
            encoding="utf-8")
        reporter = (ROOT / "tests/ci_pipeline_trace.h").read_text(encoding="utf-8")
        for family in ("raw", "harmonic", "general_baseline", "general_alt"):
            self.assertEqual(source.count('"' + family + '_compare_won"'), 1)
            self.assertEqual(source.count('"' + family + '_compare_lost"'), 1)
        self.assertIn('comparison ? "primary_compare"', reporter)
        self.assertIn('#if defined(SMART_TEMPO_CI_ORACLE_PROBE)', source)

    def test_all_four_comparator_families_and_outcome_denominators(self):
        rows = with_resource(trace())
        insert_at = next(i for i, row in enumerate(rows)
                         if row["stage"] == "primary_disposition")
        events = []
        families = ("raw", "harmonic", "general_baseline", "general_alt")
        for family in families:
            candidate = "c0" if family == "raw" else "c1"
            for score, verdict in ((.95, "won"), (.90, "lost")):
                rank = event("primary_rank", candidate, [], 128., "included",
                             family + "_rank_scored", 0)
                comp = event("primary_compare", candidate, [], 128.,
                             "included" if verdict == "won" else "excluded",
                             family + "_compare_" + verdict, 0)
                rank["metrics"]["score"] = score
                comp["metrics"]["score"] = score
                events.extend((rank, comp))
        rows[insert_at:insert_at] = events
        for index, row in enumerate(rows):
            row["sequence"] = index
        result = validate(rows)
        self.assertEqual(result["selector_comparator_observations"], 8)
        self.assertEqual(result["selector_comparator_families"], 4)
        self.assertEqual((result["selector_comparator_wins"],
                          result["selector_comparator_losses"]), (4, 4))
        for family in families:
            self.assertEqual(result["selector_compare_" + family + "_wins"], 1)
            self.assertEqual(result["selector_compare_" + family + "_losses"], 1)
        for family in families:
            for mutation in ("higher_lost", "lower_won", "missing_rank"):
                with self.subTest(family=family, mutation=mutation):
                    bad = copy.deepcopy(rows)
                    matches = [x for x in bad if x["stage"] == "primary_compare"
                               and x["reason_code"].startswith(family + "_compare_")]
                    second = matches[1]
                    if mutation == "higher_lost":
                        second["metrics"]["score"] = 1.20
                        # Preserve rank/comparator equality to isolate chronology.
                        prior = bad[bad.index(second) - 1]
                        prior["metrics"]["score"] = 1.20
                    elif mutation == "lower_won":
                        second["reason_code"] = family + "_compare_won"
                        second["outcome"] = "included"
                    else:
                        bad.remove(bad[bad.index(second) - 1])
                    for index, row in enumerate(bad):
                        row["sequence"] = index
                    with self.assertRaises(TraceError):
                        validate(bad)

    def test_ci_comparator_shadow_is_guarded_and_matches_production(self):
        source = (ROOT / "src/foo_smart_tempo/mir_candidate_pipeline.cpp").read_text(
            encoding="utf-8")
        families = ("raw", "general_baseline", "harmonic", "general_alt")
        pattern = re.compile(
            r"#if defined\(SMART_TEMPO_CI_ORACLE_PROBE\)\s+"
            r"const bool ciComparisonWon =\s*(.*?);\s*#endif\s*if\s*\(",
            re.DOTALL)

        def verify(content):
            matches = list(pattern.finditer(content))
            self.assertEqual(len(matches), 4)
            observed = set()
            for match in matches:
                pos = match.end() - 1
                depth = 0
                end = None
                for index in range(pos, len(content)):
                    char = content[index]
                    if char == "(":
                        depth += 1
                    elif char == ")":
                        depth -= 1
                        if depth == 0:
                            end = index
                            break
                self.assertIsNotNone(end)
                ci_expression = "".join(match.group(1).split())
                production_expression = "".join(content[pos + 1:end].split())
                self.assertEqual(ci_expression, production_expression)
                tail = content[end:end + 450]
                candidates = [f for f in families
                              if '"' + f + '_compare_won"' in tail
                              and '"' + f + '_compare_lost"' in tail]
                self.assertEqual(len(candidates), 1)
                observed.add(candidates[0])
                self.assertIn('ciComparisonWon ? "' + candidates[0] +
                              '_compare_won"', tail)
            self.assertEqual(observed, set(families))

        verify(source)
        # Independent negative mutations must fail before the costly SDK build.
        mutations = (
            source.replace("const bool ciComparisonWon = best == nullptr || score > bestScore;",
                           "const bool ciComparisonWon = best == nullptr || score < bestScore;", 1),
            source.replace("#endif\n    if (best == nullptr || score > bestScore) {",
                           "#endif\n    if (best == nullptr || score < bestScore) {", 1),
            source.replace("#if defined(SMART_TEMPO_CI_ORACLE_PROBE)\n    const bool ciComparisonWon",
                           "#if 0\n    const bool ciComparisonWon", 1),
            source.replace('ciComparisonWon ? "harmonic_compare_won"',
                           'false ? "harmonic_compare_won"', 1),
        )
        for mutation in mutations:
            self.assertNotEqual(mutation, source)
            with self.assertRaises(AssertionError):
                verify(mutation)

    def test_per_candidate_disposition_recomputed_from_executed_events(self):
        rows = with_resource(trace())
        self.assertEqual(validate(rows)["selector_disposition_observations"], 1)
        disposition = next(x for x in rows if x["stage"] == "primary_disposition")
        for reason in ("ranked_not_final", "observed_not_ranked",
                       "no_cluster_observation"):
            bad = copy.deepcopy(rows)
            next(x for x in bad if x["stage"] == "primary_disposition")["reason_code"] = reason
            with self.subTest(reason=reason), self.assertRaises(TraceError):
                validate(bad)
        missing = [x for x in rows if x["stage"] != "primary_disposition"]
        for i, row in enumerate(missing):
            row["sequence"] = i
        # Legacy short unit traces are allowed, but native host qualification isn't.
        self.assertEqual(validate(missing)["selector_disposition_observations"], 0)
        window = event("decode_window", None, [], 0., "observed",
                       "actual_decoded_extent", 0)
        window["metrics"] = dict(bpm=0., score=0., support=0.,
                                pass_index=0, pass_total=1, start_sec=0.,
                                requested_frames=16000, decoded_frames=16000,
                                analysis_frames=16000, source_rate_hz=16000,
                                analysis_rate_hz=16000)
        missing.insert(0, window)
        for i, row in enumerate(missing):
            row["sequence"] = i
        with self.assertRaises(TraceError):
            validate(missing, require_windows=True)

    def test_actual_decoder_window_coverage_and_overlap(self):
        rows = with_resource(trace())
        windows = []
        for index, start in enumerate((0.0, 0.5, 1.0)):
            row = event("decode_window", None, [], 0., "observed",
                        "actual_decoded_extent", 0)
            row["metrics"] = dict(bpm=0., score=0., support=0.,
                pass_index=index, pass_total=3, start_sec=start,
                requested_frames=32000, decoded_frames=32000,
                analysis_frames=32000, source_rate_hz=16000,
                analysis_rate_hz=16000)
            windows.append(row)
        rows[:0] = windows
        for index, row in enumerate(rows):
            row["sequence"] = index
        result = validate(rows, require_windows=True)
        self.assertEqual((result["decoded_windows"], result["declared_windows"],
                          result["windows_complete"]), (3, 3, 1))
        self.assertEqual(result["requested_window_seconds"], 6.)
        self.assertEqual(result["decoded_window_seconds"], 6.)
        self.assertEqual(result["unique_covered_seconds"], 3.)
        self.assertEqual(result["repeat_factor"], 2.)

    def test_window_capture_missing_partial_and_out_of_order_fail_closed(self):
        rows = with_resource(trace())
        with self.assertRaises(TraceError):
            validate(rows, require_windows=True)
        row = event("decode_window", None, [], 0., "observed",
                    "actual_decoded_extent", 0)
        row["metrics"] = dict(bpm=0., score=0., support=0.,
            pass_index=0, pass_total=2, start_sec=1.5,
            requested_frames=32000, decoded_frames=24000,
            analysis_frames=24000, source_rate_hz=16000,
            analysis_rate_hz=16000)
        rows.insert(0, row)
        for index, item in enumerate(rows):
            item["sequence"] = index
        self.assertEqual(validate(rows)["windows_complete"], 0)
        with self.assertRaises(TraceError):
            validate(rows, require_windows=True)
        rows[0]["metrics"]["pass_total"] = 1
        self.assertEqual(validate(rows, require_windows=True)["decoded_windows"], 1)
        rows[0]["metrics"]["pass_index"] = 1
        with self.assertRaises(TraceError):
            validate(rows)

    def test_decoder_window_adversarial_numeric_and_privacy_inputs(self):
        rows = with_resource(trace())
        row = event("decode_window", None, [], 0., "observed",
                    "actual_decoded_extent", 0)
        row["metrics"] = dict(bpm=0., score=0., support=0.,
            pass_index=0, pass_total=1, start_sec=0.,
            requested_frames=32000, decoded_frames=32000,
            analysis_frames=32000, source_rate_hz=16000,
            analysis_rate_hz=16000)
        rows.insert(0, row)
        for index, item in enumerate(rows):
            item["sequence"] = index
        self.assertEqual(validate(rows, require_windows=True)["windows_complete"], 1)
        for key, value in (("decoded_frames", 32001), ("analysis_frames", 50000),
                           ("source_rate_hz", 0), ("analysis_rate_hz", 96000),
                           ("pass_index", .5), ("start_sec", -1.),
                           ("start_sec", float("nan")), ("requested_frames", True),
                           ("pass_total", 129), ("score", .5)):
            with self.subTest(key=key, value=value):
                bad = copy.deepcopy(rows)
                bad[0]["metrics"][key] = value
                with self.assertRaises(TraceError):
                    validate(bad, require_windows=True)
        for field, value in (("file_path", "private.wav"),
                             ("candidate_id", "c0"),
                             ("parent_candidate_ids", ["c0"])):
            with self.subTest(field=field):
                bad = copy.deepcopy(rows)
                bad[0][field] = value
                with self.assertRaises(TraceError):
                    validate(bad, require_windows=True)

    def test_policy_same_bpm_comparison_never_proves_chosen_pointer(self):
        baseline = trace()
        result = validate(baseline)
        self.assertEqual(result["policy_output_source_comparison"], "policy_one_same_bpm")
        self.assertEqual(result["policy_pointer_identity_complete"], 0)
        self.assertEqual(baseline[6]["parent_candidate_ids"], [])
        doubled = copy.deepcopy(baseline)
        doubled.insert(4, event("policy_eligibility", "c3", ["c1"], 127.99,
                                "included", "full_board_member", 4))
        for i, row in enumerate(doubled):
            row["sequence"] = i
        doubled[6]["reason_code"] = "policy_multiple_same_bpm"
        doubled[6]["parent_candidate_ids"] = []
        self.assertEqual(validate(doubled)["policy_output_source_comparison"],
                         "policy_multiple_same_bpm")
        bad = copy.deepcopy(doubled)
        bad[6]["reason_code"] = "policy_one_same_bpm"
        bad[6]["parent_candidate_ids"] = ["c2"]
        with self.assertRaises(TraceError):
            validate(bad)
        bad = trace()
        bad[5]["reason_code"] = "policy_no_fullboard_match"
        bad[5]["parent_candidate_ids"] = []
        with self.assertRaises(TraceError):
            validate(bad)

    def test_policy_override_and_release_flag_independence(self):
        for override, release in ((1., 0.), (0., 1.), (1., 1.)):
            with self.subTest(override=override, release=release):
                rows = trace()
                rows[5]["metrics"]["score"] = override
                rows[5]["metrics"]["support"] = release
                rows[7]["reason_code"] = "writer_override" if override else "hold_release"
                measured = validate(rows)
                self.assertEqual(measured["policy_override_active"], int(override))
                self.assertEqual(measured["policy_hold_release_active"], int(release))
        invalid = trace()
        invalid[5]["metrics"]["score"] = 2.
        with self.assertRaises(TraceError):
            validate(invalid)

    def test_native_policy_object_pointer_distinguishes_numeric_coincidence(self):
        rows = trace()
        self.assertEqual(validate(rows)["policy_pointer_identity_complete"], 0)
        rows[6]["reason_code"] = "pointer_native_fullboard_consistent"
        rows[6]["parent_candidate_ids"] = ["c2"]
        rows[6]["metrics"]["score"] = 1.
        rows[6]["metrics"]["support"] = 1.
        rows[7]["parent_candidate_ids"] = ["c2"]
        qualified = validate(rows)
        self.assertEqual(qualified["policy_pointer_identity_complete"], 1)
        self.assertEqual(qualified["policy_pointer_reason"],
                         "pointer_native_fullboard_consistent")
        for change in ("bad_parent", "invented_pointer", "different_policy", "foreign_id"):
            with self.subTest(change=change):
                bad = copy.deepcopy(rows)
                if change == "bad_parent":
                    bad[6]["parent_candidate_ids"] = ["c0"]
                elif change == "invented_pointer":
                    bad[6]["metrics"]["score"] = 0.
                elif change == "different_policy":
                    bad[7]["parent_candidate_ids"] = []
                else:
                    bad[6]["parent_candidate_ids"] = ["c99"]
                with self.assertRaises(TraceError):
                    validate(bad)

    def test_unobserved_supplemental_policy_cannot_invent_fullboard_pointer(self):
        rows = trace()
        rows[6]["reason_code"] = "pointer_supplemental_or_changed"
        rows[6]["metrics"]["score"] = 1.
        self.assertEqual(validate(rows)["policy_pointer_identity_complete"], 0)
        bad = copy.deepcopy(rows)
        bad[6]["parent_candidate_ids"] = ["c2"]
        with self.assertRaises(TraceError):
            validate(bad)

    def test_resource_provenance_is_process_scoped_and_fail_closed(self):
        rows = with_resource(trace())
        self.assertEqual(validate(rows)["process_cpu_seconds"], 3.75)
        with self.assertRaises(TraceError):
            validate(trace(), require_windows=True)
        cases = (
            ("wall_seconds", -1.), ("wall_seconds", float("nan")),
            ("process_cpu_seconds", float("inf")),
            ("process_cpu_seconds", 2000.),
            ("process_peak_rss_after_bytes", 1),
            ("process_peak_rss_after_bytes", 4.5),
            ("process_working_set_after_bytes", 47_000_000),
            ("score", 1.), ("path", "private")
        )
        for key, value in cases:
            with self.subTest(key=key):
                bad = copy.deepcopy(rows)
                bad[-2]["metrics"][key] = value
                with self.assertRaises(TraceError):
                    validate(bad)
        bad = copy.deepcopy(rows)
        bad.insert(-1, resource_event())
        for i, row in enumerate(bad):
            row["sequence"] = i
        with self.assertRaises(TraceError):
            validate(bad)

    def test_supplemental_selected_object_identity_rejects_bpm_substitution(self):
        rows = trace()
        rows.insert(4, event("supplemental_candidate", "c3", [], 127.99,
                             "observed", "partial_bar_topk_source", 0))
        rows[7]["reason_code"] = "pointer_native_supplemental_consistent"
        rows[7]["parent_candidate_ids"] = ["c3"]
        rows[7]["metrics"]["score"] = 1.
        rows[8]["parent_candidate_ids"] = ["c3"]
        rows[10]["parent_candidate_ids"] = ["c3"]
        for i, row in enumerate(rows):
            row["sequence"] = i
        result = validate(rows)
        self.assertEqual(result["policy_supplemental_pointer_identity_complete"], 1)
        self.assertEqual(result["supplemental_selected_sources"], 1)
        self.assertEqual(result["indexed_supplemental_final_witness"], 1)
        for key in ("wrong_id", "lost_source", "wrong_board_flag",
                    "wrong_bpm", "foreign_kind", "duplicate",
                    "witness_source_lost", "witness_source_forged",
                    "unindexed_selected_source"):
            with self.subTest(key=key):
                bad = copy.deepcopy(rows)
                if key == "wrong_id":
                    bad[7]["parent_candidate_ids"] = ["c2"]
                    bad[8]["parent_candidate_ids"] = ["c2"]
                elif key == "lost_source": bad.pop(4)
                elif key == "wrong_board_flag": bad[7]["metrics"]["support"] = 1.
                elif key == "wrong_bpm": bad[4]["metrics"]["bpm"] = 140.
                elif key == "foreign_kind": bad[4]["reason_code"] = "reference_bpm_source"
                elif key == "duplicate":
                    duplicate = copy.deepcopy(bad[4])
                    duplicate["candidate_id"] = "c4"
                    bad.insert(5, duplicate)
                elif key == "witness_source_lost":
                    bad[10]["parent_candidate_ids"] = ["c2"]
                elif key == "unindexed_selected_source":
                    bad[10]["parent_candidate_ids"] = []
                    bad[10]["reason_code"] = "supplemental_witness_unindexed"
                elif key == "witness_source_forged":
                    bad[10]["parent_candidate_ids"] = ["c2", "c3"]
                    bad[7]["reason_code"] = "pointer_not_observed"
                    bad[7]["parent_candidate_ids"] = []
                    bad[7]["metrics"]["score"] = 0.
                    bad[8]["parent_candidate_ids"] = []
                for i, row in enumerate(bad):
                    row["sequence"] = i
                with self.assertRaises(TraceError):
                    validate(bad)

    def test_fullboard_pointer_cannot_launder_supplemental_final_witness(self):
        rows = trace()
        rows.insert(4, event("supplemental_candidate", "c3", [], 127.99,
                             "observed", "partial_bar_topk_source", 0))
        rows[10]["parent_candidate_ids"] = ["c2", "c3"]
        for i, row in enumerate(rows):
            row["sequence"] = i
        with self.assertRaises(TraceError):
            validate(rows)

    def test_native_selector_capture_fails_closed_on_loss(self):
        """CI capture overflow/OOM must never be reported as complete provenance."""
        source = (ROOT / "src" / "foo_smart_tempo" /
                  "mir_candidate_pipeline.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src" / "foo_smart_tempo" /
                  "mir_candidate_pipeline.h").read_text(encoding="utf-8")
        component = (ROOT / "src" / "foo_smart_tempo" /
                     "modern_bpm_analyzer.cpp").read_text(encoding="utf-8")
        self.assertIn("mir_ci_primary_selector_trace_complete() noexcept", header)
        self.assertIn("g_ci_primary_events.size() >= 4096", source)
        self.assertEqual(source.count("g_ci_primary_trace_complete = false;"), 3)
        self.assertEqual(source.count("g_ci_primary_trace_complete = true;"), 2)
        self.assertIn("g_ci_primary_events.clear();\n  g_ci_primary_pool.clear();\n  g_ci_primary_trace_complete = true;", source)
        self.assertIn("mir_ci_primary_selector_trace_complete() &&", component)
        self.assertLess(component.index("mir_ci_primary_selector_trace_complete() &&"),
                        component.index("smart_tempo::ci_pipeline_trace::record("))
        self.assertIn("#if defined(SMART_TEMPO_CI_ORACLE_PROBE)", source)

    def test_ci_only_compilation_and_host_provenance_chain(self):
        component = (ROOT / "src" / "foo_smart_tempo" /
                     "modern_bpm_analyzer.cpp").read_text(encoding="utf-8")
        header = (ROOT / "tests" / "ci_pipeline_trace.h").read_text(encoding="utf-8")
        workflow = (ROOT / ".github" / "workflows" /
                    "source.yml").read_text(encoding="utf-8")
        host = (ROOT / "tests" / "diagnostic_live_host_smoke.ps1").read_text(
            encoding="utf-8")
        self.assertIn("#ifndef SMART_TEMPO_CI_ORACLE_PROBE", header)
        self.assertIn("#if defined(SMART_TEMPO_CI_ORACLE_PROBE)", component)
        self.assertIn("ci_pipeline_trace::record(", component)
        self.assertIn("ciWindows.push_back", component)
        self.assertIn("ciWindowCaptureFailed", component)
        self.assertIn('out.push_window(w)', header)
        self.assertIn('"actual_decoded_extent"', header)
        self.assertIn("--require-windows", host)
        self.assertIn('L"GITHUB_ACTIONS"', header)
        self.assertIn('L"RUNNER_TEMP"', header)
        self.assertIn('L"SMART_TEMPO_CI_FIXTURE"', header)
        self.assertIn('L"\\synthetic-oracle\\pulse_128.wav"', header.replace('\\\\', '\\'))
        self.assertIn("CREATE_NEW", header)
        self.assertIn("mir_ci_primary_selector_pool()", component)
        self.assertIn("ci_run_id()", header)
        self.assertIn("same_run_measured_witness", header)
        self.assertIn('out.push("measured_witness"', header)
        self.assertIn('out.push("policy_resolution"', header)
        self.assertIn('out.push("policy_pointer"', header)
        self.assertIn("mir_ci_policy_pointer_source()", component)
        source = (ROOT / "src" / "foo_smart_tempo" /
                  "mir_candidate_pipeline.cpp").read_text(encoding="utf-8")
        self.assertIn("ci_policy_selected(selected)", source)
        self.assertIn("g_ci_policy_board = candidates", source)
        self.assertIn("MirCiSupplementalKind::partial_bar_topk, selected);", source)
        self.assertIn("MirCiSupplementalKind::material_risk, materialRiskProbe);", source)
        self.assertIn("MirCiSupplementalKind::partial_bar_aggregate, partialBarAggregate);", source)
        schema = json.loads((ROOT / "tools" /
            "smart_tempo_decision_trace_v1.schema.json").read_text(encoding="utf-8"))
        self.assertTrue(set(ENUMS).issubset(set(schema["properties"]["stage"]["enum"])))
        self.assertNotIn("std::isfinite(value) ? value : 0.0", header)
        self.assertIn("pool_only_gate_unresolved", header)
        self.assertIn("mir_ci_primary_selector_events()", component)
        self.assertIn("candidate_filter", header)
        self.assertIn("primary_rank", header)
        self.assertIn("item.source_index >= count", header)
        self.assertIn("primary_parent.push_back(cluster_ids[item.source_index])", header)
        self.assertIn("terminal_final_choice", header)
        self.assertIn("ids[item.source_index]", header)
        self.assertIn('"primary_terminal"', header)
        self.assertIn("general_baseline_rank_scored", header)
        self.assertNotIn("std::size_t match = count", header)
        selector_header = (ROOT / "src" / "foo_smart_tempo" /
                           "mir_candidate_pipeline.h").read_text(encoding="utf-8")
        self.assertIn("std::size_t source_index = 0", selector_header)
        selector = (ROOT / "src" / "foo_smart_tempo" /
                    "mir_candidate_pipeline.cpp").read_text(encoding="utf-8")
        self.assertIn("candidate - board.raw_candidates.data()", selector)
        self.assertIn("&candidate - candidates.data()", selector)
        for snippet in ("raw_alias_provisional_exclusion", "raw_support_provisional_exclusion",
                        "low_upper_gate_provisional_exclusion", "low_threshold_allowed",
                        "harmonic_rank_scored", "general_out_of_rail_provisional",
                        "general_alt_rank_scored", "low_continuous_allowed",
                        "terminal_lane_choice", "terminal_residual_choice",
                        "terminal_final_choice"):
            self.assertIn(snippet, selector)
        self.assertIn("audit_smart_tempo_ci_pipeline_trace.py --trace", host)
        self.assertIn("test_smart_tempo_ci_pipeline_trace.py", workflow)
        self.assertNotRegex(workflow, r"(?m)^\s*uses:\s*actions/upload-artifact@")


if __name__ == "__main__":
    unittest.main()
