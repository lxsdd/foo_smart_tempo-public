#!/usr/bin/env python3
"""Adversarial audit of the *actual* CI-only foobar native trace.

Every mutation is applied to the original host's parsed JSONL, not to a
hand-picked surrogate. The same strict parser and source-ID fate replay must
reject each corrupted copy. No audio, oracle, tag, path or metadata is read,
and no trace is written back to disk. This proves rejection coverage, NOT
musical accuracy or complete first-decisive-veto provenance.
"""
from __future__ import annotations

import argparse
import copy
from pathlib import Path

from audit_smart_tempo_ci_pipeline_trace import TraceError, load, validate
from audit_smart_tempo_b_candidate_fates import reconcile


def mutations(rows: list[dict]) -> list[tuple[str, list[dict]]]:
    """Deterministic privacy-safe corruptions of actual run-local source IDs."""
    result: list[tuple[str, list[dict]]] = []

    def add(name, predicate, change, *, required=False):
        index = next((i for i, row in enumerate(rows) if predicate(row)), None)
        if index is None:
            if required:
                raise TraceError("native mutation source missing: " + name)
            return
        altered = copy.deepcopy(rows)
        change(altered, index)
        if altered == rows:
            raise TraceError("native mutation did not change evidence: " + name)
        result.append((name, altered))

    def row_stage(stage):
        return lambda row: row["stage"] == stage

    add("sequence_gap", lambda row: True,
        lambda a, i: a[i].__setitem__("sequence", -1), required=True)
    add("schema_spoof", lambda row: True,
        lambda a, i: a[i].__setitem__("schema", "unknown_trace"), required=True)
    add("foreign_analysis", lambda row: True,
        lambda a, i: a[i].__setitem__(
            "analysis_id", "ffffffffffffffff"
            if a[i]["analysis_id"] != "ffffffffffffffff" else "0000000000000000"),
        required=True)
    add("privacy_field", lambda row: True,
        lambda a, i: a[i].__setitem__("track_path", "forbidden"),
        required=True)
    add("window_index", row_stage("decode_window"),
        lambda a, i: a[i]["metrics"].__setitem__("pass_index", -1),
        required=True)
    add("window_overdecode", row_stage("decode_window"),
        lambda a, i: a[i]["metrics"].__setitem__(
            "decoded_frames", a[i]["metrics"]["requested_frames"] + 1),
        required=True)
    add("mir_nonfinite", row_stage("mir_segment"),
        lambda a, i: a[i]["metrics"].__setitem__("score", float("nan")),
        required=True)
    add("negative_wall_time", row_stage("resource_usage"),
        lambda a, i: a[i]["metrics"].__setitem__("wall_seconds", -1.0),
        required=True)
    add("invalid_rss", row_stage("resource_usage"),
        lambda a, i: a[i]["metrics"].__setitem__(
            "process_peak_rss_after_bytes", 0), required=True)
    add("inventory_raw_count", row_stage("source_inventory"),
        lambda a, i: a[i]["metrics"].__setitem__(
            "raw_count", a[i]["metrics"]["raw_count"] + 1), required=True)
    add("inventory_window_count", row_stage("source_inventory"),
        lambda a, i: a[i]["metrics"].__setitem__(
            "decode_window_count", a[i]["metrics"]["decode_window_count"] + 1),
        required=True)
    add("writer_bypass", row_stage("write_guard"),
        lambda a, i: a[i].__setitem__("outcome", "selected"), required=True)
    add("fabricated_final", row_stage("final_output"),
        lambda a, i: a[i].__setitem__("outcome", "observed"), required=True)
    add("fabricated_witness", row_stage("measured_witness"),
        lambda a, i: a[i].__setitem__("outcome", "observed"), required=True)
    add("policy_override_flag", row_stage("policy_resolution"),
        lambda a, i: a[i]["metrics"].__setitem__("score", 2), required=True)
    add("policy_pointer_flag", row_stage("policy_pointer"),
        lambda a, i: a[i]["metrics"].__setitem__("support", 2), required=True)
    if any(row["stage"] == "writer_resolution" for row in rows):
        add("writer_reason_spoof", row_stage("writer_resolution"),
            lambda a, i: a[i].__setitem__("reason_code", "mir_unregistered"),
            required=True)
        add("writer_runtime_spoof", row_stage("writer_resolution"),
            lambda a, i: a[i]["metrics"].__setitem__("score", 2),
            required=True)
        add("writer_policy_flag_spoof", row_stage("writer_resolution"),
            lambda a, i: a[i]["metrics"].__setitem__("support", 2),
            required=True)
        add("writer_bpm_spoof", row_stage("writer_resolution"),
            lambda a, i: a[i]["metrics"].__setitem__(
                "bpm", a[i]["metrics"]["bpm"] + 1),
            required=True)
        add("writer_parent_spoof", row_stage("writer_resolution"),
            lambda a, i: a[i].__setitem__(
                "parent_candidate_ids", [] if a[i]["parent_candidate_ids"]
                else ["c999999"]),
            required=True)
        add("writer_outcome_spoof", row_stage("writer_resolution"),
            lambda a, i: a[i].__setitem__(
                "outcome", "review_hold" if a[i]["outcome"] == "selected"
                else "selected"), required=True)
        missing = [r for r in copy.deepcopy(rows)
                   if r["stage"] != "writer_resolution"]
        for i, row in enumerate(missing):
            row["sequence"] = i
        result.append(("missing_writer_resolution", missing))
    if any(row["stage"] == "candidate_transform" for row in rows):
        # Actual-host first-exclusion proof must be present for every
        # collapsed source, including sources never checked by this lane.
        add("first_exclusion_source", row_stage("primary_first_exclusion"),
            lambda a, i: a[i].__setitem__("candidate_id", "c999999"),
            required=True)
        add("first_exclusion_reason", row_stage("primary_first_exclusion"),
            lambda a, i: a[i].__setitem__(
                "reason_code", "harmonic_out_of_rail"
                if a[i]["reason_code"] == "no_native_exclusion_observed"
                else "no_native_exclusion_observed"), required=True)
        add("first_exclusion_index", row_stage("primary_first_exclusion"),
            lambda a, i: a[i]["metrics"].__setitem__(
                "support", a[i]["metrics"]["support"] + 1), required=True)
        add("first_exclusion_score", row_stage("primary_first_exclusion"),
            lambda a, i: a[i]["metrics"].__setitem__(
                "score", a[i]["metrics"]["score"] + 1), required=True)
        add("first_exclusion_parent", row_stage("primary_first_exclusion"),
            lambda a, i: a[i].__setitem__("parent_candidate_ids", ["c0"]),
            required=True)
        for label, changed in (
            ("missing_first_exclusion",
             [r for r in copy.deepcopy(rows)
              if r["stage"] != "primary_first_exclusion"]),
            ("duplicate_first_exclusion", None),
        ):
            if changed is None:
                changed = copy.deepcopy(rows)
                pos = next(i for i, row in enumerate(changed)
                           if row["stage"] == "primary_first_exclusion")
                changed.insert(pos, copy.deepcopy(changed[pos]))
            for i, row in enumerate(changed):
                row["sequence"] = i
            result.append((label, changed))
    add("unknown_raw_origin", row_stage("candidate_create"),
        lambda a, i: a[i].__setitem__("reason_code", "unregistered_raw_source"))
    add("orphan_cluster", row_stage("candidate_transform"),
        lambda a, i: a[i].__setitem__("parent_candidate_ids", []))
    add("wrong_disposition_source", row_stage("primary_disposition"),
        lambda a, i: a[i].__setitem__("candidate_id", "c999999"))
    add("wrong_rank_source", row_stage("primary_rank"),
        lambda a, i: a[i].__setitem__("candidate_id", "c999999"))
    add("wrong_terminal_source", row_stage("primary_terminal"),
        lambda a, i: a[i].__setitem__("candidate_id", "c999999"))
    add("comparator_verdict", row_stage("primary_compare"),
        lambda a, i: a[i].__setitem__(
            "outcome", "excluded" if a[i]["outcome"] == "included" else "included"))
    add("unknown_filter", row_stage("candidate_filter"),
        lambda a, i: a[i].__setitem__("reason_code", "unregistered_filter"))
    add("orphan_policy_board", row_stage("policy_eligibility"),
        lambda a, i: a[i].__setitem__("parent_candidate_ids", []))
    add("policy_rail_wrong_source", row_stage("policy_filter"),
        lambda a, i: a[i].__setitem__("candidate_id", "c999999"))
    add("policy_rail_false_verdict", row_stage("policy_filter"),
        lambda a, i: a[i].__setitem__("outcome",
            "excluded" if a[i]["outcome"] == "included" else "included"))
    if any(row["stage"] == "policy_filter" for row in rows):
        altered = [x for x in copy.deepcopy(rows) if x["stage"] != "policy_filter"]
        for i, row in enumerate(altered):
            row["sequence"] = i
        result.append(("missing_policy_rail", altered))
    add("unknown_supplemental", row_stage("supplemental_candidate"),
        lambda a, i: a[i].__setitem__("reason_code", "unregistered_supplemental"))
    add("decisive_veto_flip", lambda row: (
        row["stage"] == "candidate_filter" and
        row["reason_code"] in (
            "raw_alias_decisive_veto", "raw_support_decisive_veto",
            "harmonic_rail_decisive_veto", "general_rail_decisive_veto")),
        lambda a, i: a[i].__setitem__("outcome", "included"))
    add("duplicate_raw_identity", lambda row: (
        row["stage"] == "candidate_create" and
        sum(x["stage"] == "candidate_create" for x in rows) > 1),
        lambda a, i: a[i+1].__setitem__("candidate_id", a[i]["candidate_id"])
        if a[i+1]["stage"] == "candidate_create" else
        a[i].__setitem__("candidate_id", "c999999"))
    # Delete a real terminal observation, renumber to avoid passing only
    # because of the trivial sequence gap.
    for stage in ("resource_usage", "write_guard", "source_inventory"):
        index = next((i for i, row in enumerate(rows) if row["stage"] == stage), None)
        if index is None:
            raise TraceError("missing required native terminal: " + stage)
        altered = copy.deepcopy(rows)
        del altered[index]
        for i, row in enumerate(altered):
            row["sequence"] = i
        result.append(("missing_" + stage, altered))
    names = [name for name, _ in result]
    if len(names) != len(set(names)) or len(names) < 19:
        raise TraceError("incomplete or duplicate native adversarial inventory")
    return result


def audit(rows: list[dict], *, native_comparator: bool,
          native_policy_rail: bool = False) -> tuple[int, int]:
    validate(rows, require_windows=True, require_segments=True,
             require_native_comparator=native_comparator,
             require_policy_filter=native_policy_rail,
             require_writer_resolution=native_policy_rail)
    reconcile(rows)
    mutated = mutations(rows)
    for name, altered in mutated:
        try:
            validate(altered, require_windows=True, require_segments=True,
                     require_native_comparator=native_comparator,
                     require_policy_filter=native_policy_rail,
             require_writer_resolution=native_policy_rail)
            reconcile(altered)
        except TraceError:
            continue
        raise TraceError("native corruption escaped independent auditors: " + name)
    return len(mutated), len(mutated) - 19


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--trace", type=Path, required=True)
    p.add_argument("--require-windows", action="store_true")
    args = p.parse_args()
    if not args.require_windows:
        raise TraceError("native adversarial audit requires original-host mode")
    rejected, extra = audit(load(args.trace), native_comparator=True,
                            native_policy_rail=True)
    print("SMART_TEMPO_NATIVE_MUTATION_GATE=PASS")
    print(f"SMART_TEMPO_NATIVE_MUTATIONS_REJECTED={rejected}")
    print(f"SMART_TEMPO_NATIVE_MUTATIONS_ADDITIONAL={extra}")


if __name__ == "__main__":
    main()
