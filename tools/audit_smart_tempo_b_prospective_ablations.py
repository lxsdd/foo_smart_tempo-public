#!/usr/bin/env python3
"""Strict synthetic-only post-host four-model prospective cohort accounting.

Musical errors are measured, not made into a technical CI failure.
No media, paths, candidate BPMs, genre tags or owner reference data emitted.
"""
import argparse
import json
import re
from pathlib import Path

CASES = (
    "probe_72_regular", "probe_104_offbeat", "probe_132_swing",
    "probe_156_half_pulse", "probe_90_polyrhythm", "probe_150_sparse",
)
MODELS = ("NATIVE_FINAL", "PRIMARY_ONLY", "COMBINED", "AGREEMENT_ONLY")
FIELDS = ("ACCEPTED", "HELD", "CORRECT_005", "CORRECT_020", "WRONG_AUTO", "FALSE_ACCEPT")
KEYS = {model + "_" + field for model in MODELS for field in FIELDS}
PAIR_FIELDS = tuple(side + "_" + field for field in
                    ("CORRECT_005", "CORRECT_020", "WRONG_AUTO", "HELD")
                    for side in ("GAIN", "LOSS"))


# Causal categories are joined only after the unchanged native analysis and
# independent source-ID comparator audit have completed. These are diagnostic
# outcomes, never an oracle-derived production selector.
RECALL_NUMERIC = (
    "RAW_NEAR_020", "CLUSTER_NEAR_020", "POLICY_BOARD_NEAR_020",
    "PRIMARY_NEAR_020", "POLICY_NEAR_020", "FINAL_ACCEPTED_NEAR_020",
)
RECALL_FIELDS = set(RECALL_NUMERIC) | {
    "ROOT_CAUSE_STAGE", "PRIMARY_COMPARATOR_DIAGNOSIS",
}
ROOT_REASONS = (
    "reference_in_selected_output", "missing_raw_reference",
    "missing_cluster_reference", "missing_policy_board_reference",
    "primary_selected_other", "policy_selected_other",
    "final_selected_other", "reference_present_held",
)
COMPARATOR_REASONS = (
    "reference_primary_selected", "reference_missing_cluster",
    "reference_not_ranked", "reference_ranked_lost",
    "reference_won_nonterminal_family",
)


class ProspectiveError(ValueError):
    pass


def validate(reports, expected_source_sha=None, require_both=True):
    if type(reports) is not list or len(reports) != (2 if require_both else 1):
        raise ProspectiveError("missing host architecture")
    if expected_source_sha is not None and not re.fullmatch(r"[a-f0-9]{40}", expected_source_sha):
        raise ProspectiveError("invalid expected source SHA")
    seen, shas, totals, paired, gates, causal = set(), set(), {}, {}, {}, {}
    reference_cases = None
    for report in reports:
        if type(report) is not dict or set(report) != {"schema", "architecture", "source_sha", "cases"}:
            raise ProspectiveError("unknown report fields")
        if report["schema"] != "smart_tempo_b_prospective_ablation_v1":
            raise ProspectiveError("unknown report schema")
        arch = report["architecture"]
        if arch not in ("Win32", "x64") or arch in seen:
            raise ProspectiveError("duplicate/unknown host architecture")
        seen.add(arch)
        sha = report["source_sha"]
        if type(sha) is not str or not re.fullmatch(r"[a-f0-9]{40}", sha):
            raise ProspectiveError("invalid source SHA")
        if expected_source_sha is not None and sha != expected_source_sha:
            raise ProspectiveError("stale source SHA")
        shas.add(sha)
        cases = report["cases"]
        if type(cases) is not list or len(cases) != len(CASES):
            raise ProspectiveError("missing prospective cases")
        aggregate = {model: {field: 0 for field in FIELDS} for model in MODELS}
        root_counts = {reason: 0 for reason in ROOT_REASONS}
        comparator_counts = {reason: 0 for reason in COMPARATOR_REASONS}
        for i, row in enumerate(cases):
            if type(row) is not dict or set(row) != {"case", "ablation", "recall", "media_no_write"}:
                raise ProspectiveError("unknown case fields")
            if row["case"] != CASES[i] or row["media_no_write"] is not True:
                raise ProspectiveError("case order/identity or no-write drift")
            counts = row["ablation"]
            if type(counts) is not dict or set(counts) != KEYS:
                raise ProspectiveError("incomplete model evidence")
            for model in MODELS:
                values = {field: counts[model + "_" + field] for field in FIELDS}
                if any(type(value) is not int or value not in (0, 1) for value in values.values()):
                    raise ProspectiveError("nonbinary model count")
                if (values["ACCEPTED"] + values["HELD"] != 1 or
                    values["CORRECT_005"] > values["CORRECT_020"] or
                    values["CORRECT_020"] + values["WRONG_AUTO"] != values["ACCEPTED"] or
                    values["FALSE_ACCEPT"] != 0):
                    raise ProspectiveError("contradictory decision/accuracy count")
                for field, value in values.items():
                    aggregate[model][field] += value
            recall = row["recall"]
            if type(recall) is not dict or set(recall) != RECALL_FIELDS:
                raise ProspectiveError("missing or extra causal source evidence")
            reason = recall["ROOT_CAUSE_STAGE"]
            comparator = recall["PRIMARY_COMPARATOR_DIAGNOSIS"]
            if reason not in ROOT_REASONS or comparator not in COMPARATOR_REASONS:
                raise ProspectiveError("unrecognized causal source diagnosis")
            if any(type(recall[key]) is not int or recall[key] < 0
                   for key in RECALL_NUMERIC):
                raise ProspectiveError("invalid causal source denominator")
            for key in ("PRIMARY_NEAR_020", "POLICY_NEAR_020",
                        "FINAL_ACCEPTED_NEAR_020"):
                if recall[key] > 1:
                    raise ProspectiveError("nonbinary causal terminal")
            correct = counts["NATIVE_FINAL_CORRECT_020"]
            if recall["FINAL_ACCEPTED_NEAR_020"] != correct or (
                (reason == "reference_in_selected_output") != bool(correct)
            ):
                raise ProspectiveError("causal diagnosis disagrees with native final")
            if ((reason == "missing_raw_reference" and recall["RAW_NEAR_020"]) or
                (reason == "missing_cluster_reference" and recall["CLUSTER_NEAR_020"]) or
                (reason == "missing_policy_board_reference" and recall["POLICY_BOARD_NEAR_020"])):
                raise ProspectiveError("causal stage contradicts measured source presence")
            root_counts[reason] += 1
            comparator_counts[comparator] += 1
        # A matching aggregate can hide opposite per-case errors. Demand
        # exact paired host outcomes, not merely matching column totals.
        if reference_cases is None:
            reference_cases = cases
        elif cases != reference_cases:
            raise ProspectiveError("cross-architecture case-level drift")
        totals[arch] = aggregate
        causal[arch] = {"root": root_counts, "comparator": comparator_counts}
        paired[arch], gates[arch] = {}, {}
        native = aggregate["NATIVE_FINAL"]
        for model in MODELS[1:]:
            delta = {field: 0 for field in PAIR_FIELDS}
            for case in cases:
                values = case["ablation"]
                for field in ("CORRECT_005", "CORRECT_020", "WRONG_AUTO", "HELD"):
                    before = values["NATIVE_FINAL_" + field]
                    after = values[model + "_" + field]
                    delta["GAIN_" + field] += int(before == 0 and after == 1)
                    delta["LOSS_" + field] += int(before == 1 and after == 0)
            paired[arch][model] = delta
            alt = aggregate[model]
            if alt["WRONG_AUTO"] > native["WRONG_AUTO"] and alt["CORRECT_020"] <= native["CORRECT_020"]:
                gate = "SYNTHETIC_RISK_REGRESSION"
            elif alt["ACCEPTED"] == 0:
                gate = "ZERO_AUTO_COVERAGE"
            elif alt["CORRECT_020"] <= native["CORRECT_020"]:
                gate = "NO_SYNTHETIC_ACCURACY_GAIN"
            else:
                gate = "REQUIRES_INDEPENDENT_MUSIC_QA"
            gates[arch][model] = gate
    if len(shas) != 1:
        raise ProspectiveError("mixed product source SHAs")
    return {"source_sha": next(iter(shas)), "architectures": totals,
            "n_per_architecture": len(CASES), "paired": paired,
            "gates": gates, "causal": causal,
            "cross_arch_case_parity": len(seen) == 2,
            "production_admission": "NO_GO"}


def unique_object(pairs):
    obj = {}
    for key, value in pairs:
        if key in obj:
            raise ProspectiveError("duplicate JSON field")
        obj[key] = value
    return obj


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", action="append", type=Path, required=True)
    parser.add_argument("--expected-source-sha", required=True)
    parser.add_argument("--single-architecture", action="store_true")
    args = parser.parse_args()
    try:
        reports = []
        for path in args.report:
            if not path.is_file() or not 0 < path.stat().st_size < 32768:
                raise ProspectiveError("missing/oversized prospective report")
            reports.append(json.loads(path.read_text(encoding="utf-8"),
                                      object_pairs_hook=unique_object))
        result = validate(reports, args.expected_source_sha,
                          require_both=not args.single_architecture)
    except (OSError, ValueError, TypeError, KeyError, UnicodeError) as exc:
        parser.error(str(exc))
    print("SMART_TEMPO_PROSPECTIVE_ABLATIONS_VERIFIED=1")
    print("SMART_TEMPO_PROSPECTIVE_SYNTHETIC_ONLY=1")
    print("SMART_TEMPO_PROSPECTIVE_N_PER_ARCH=" + str(result["n_per_architecture"]))
    print("SMART_TEMPO_PROSPECTIVE_CROSS_ARCH_CASE_PARITY=" +
          str(int(result["cross_arch_case_parity"])))
    print("SMART_TEMPO_PROSPECTIVE_PRODUCTION_ADMISSION=" +
          result["production_admission"])
    for arch in ("Win32", "x64"):
        if arch not in result["architectures"]:
            continue
        for family, reasons in (("ROOT", ROOT_REASONS),
                                ("COMPARATOR", COMPARATOR_REASONS)):
            for reason in reasons:
                print("SMART_TEMPO_PROSPECTIVE_" + arch.upper() + "_CAUSAL_" +
                      family + "_" + reason.upper() + "=" +
                      str(result["causal"][arch][family.lower()][reason]))
        for model in MODELS:
            for field in FIELDS:
                print("SMART_TEMPO_PROSPECTIVE_" + arch.upper() + "_" + model + "_" + field +
                      "=" + str(result["architectures"][arch][model][field]))
        for model in MODELS[1:]:
            print("SMART_TEMPO_PROSPECTIVE_" + arch.upper() + "_" + model +
                  "_GATE=" + result["gates"][arch][model])
            for field in PAIR_FIELDS:
                print("SMART_TEMPO_PROSPECTIVE_" + arch.upper() + "_" + model +
                      "_PAIRED_" + field + "=" + str(result["paired"][arch][model][field]))


if __name__ == "__main__":
    main()
