#!/usr/bin/env python3
"""Read-only CI-only numeric origin masks for fixed public synthetic candidate exports.

Bit 1 full_board, 2 primary_selector, 4 material_risk_probe,
8 partial_bar_aggregate, 16 partial_bar_topk. These are merged-review
provenance marks, NOT primary eligibility, root causes or BPM proposals.
"""
from __future__ import annotations

import csv
import math
from pathlib import Path

FIELDS = ("id", "rank", "bpm", "source_bits")
FULL, PRIMARY = 1, 2
COUNTS = ("wrong", "precise", "precise_missing", "precise_full",
          "precise_primary", "precise_supplement_only", "output",
          "output_missing", "output_full", "output_primary",
          "output_supplement_only")
RISKS = frozenset(("halftrap", "doubletrap", "switch", "nonperiodic", "tone"))


def load_source_bits(path: Path, names: list[str],
                     signals: dict[str, list[dict]]) -> dict[str, list[int]]:
    if (len(names) != 16 or len(set(names)) != 16 or set(names) != set(signals) or
            any(not name.startswith("challenge_") for name in names)):
        raise ValueError("invalid fixed public source-bit inventory")
    expected = [(name, row["rank"], row["bpm"])
                for name in names for row in signals[name]]
    if len(expected) > 16 * 32:
        raise ValueError("source-bit inventory exceeds cap")
    with path.open(encoding="utf-8-sig", newline="") as file:
        reader = csv.DictReader(file)
        if tuple(reader.fieldnames or ()) != FIELDS:
            raise ValueError("source-bit schema mismatch")
        rows = list(reader)
    if len(rows) != len(expected):
        raise ValueError("source-bit candidate count mismatch")
    result: dict[str, list[int]] = {name: [] for name in names}
    for (name, rank, bpm), row in zip(expected, rows):
        if set(row) != set(FIELDS) or (row["id"], row["rank"], row["bpm"]) != (
                name, rank, bpm):
            raise ValueError("source-bit ordered rank/BPM mismatch")
        value = row["source_bits"]
        if (not isinstance(value, str) or not value.isascii() or
                not value.isdecimal() or value.startswith("0") or
                not 1 <= int(value) <= 31):
            raise ValueError("unapproved numeric provenance mask")
        result[name].append(int(value))
    return result


def audit_source_bits(scored: dict, signals: dict[str, list[dict]],
                      stages: dict, masks: dict[str, list[int]], cases: tuple) -> dict:
    names = [name for name, _, _ in cases]
    if (len(names) != 16 or len(set(names)) != 16 or
            set(names) != set(signals) or set(names) != set(masks) or
            [row["id"] for row in scored["results"]] != names or
            [row[0] for row in stages["details"]] != names):
        raise ValueError("source-bit stage/oracle identity mismatch")
    counts = {stage: dict.fromkeys(COUNTS, 0) for stage in stages["stages"]}
    cohort_fields = ("cases", "cases_with_board", "candidate_rows",
                     "candidate_full_rows", "candidate_primary_rows",
                     "candidate_supplement_only_rows", "cases_with_full",
                     "cases_with_primary", "cases_with_supplement_only")
    cohorts = {"clear": dict.fromkeys(cohort_fields, 0),
               "risk": dict.fromkeys(cohort_fields, 0)}
    details = []
    for (name, reference, style), observed, info in zip(
            cases, scored["results"], stages["details"]):
        _, stage, outcome, _, _ = info
        if stage not in counts:
            raise ValueError("source-bit unknown final stage")
        board, bits = signals[name], masks[name]
        if len(board) != len(bits) or len(board) > 32 or any(
                type(m) is not int or not 1 <= m <= 31 for m in bits):
            raise ValueError("source-bit board mismatch")
        cohort = cohorts["risk" if style in RISKS else "clear"]
        cohort["cases"] += 1
        cohort["cases_with_board"] += int(bool(bits))
        cohort["candidate_rows"] += len(bits)
        cohort["candidate_full_rows"] += sum(bool(m & FULL) for m in bits)
        cohort["candidate_primary_rows"] += sum(bool(m & PRIMARY) for m in bits)
        cohort["candidate_supplement_only_rows"] += sum(
            not (m & (FULL | PRIMARY)) for m in bits)
        cohort["cases_with_full"] += int(any(m & FULL for m in bits))
        cohort["cases_with_primary"] += int(any(m & PRIMARY for m in bits))
        cohort["cases_with_supplement_only"] += int(
            any(not (m & (FULL | PRIMARY)) for m in bits))
        if style in RISKS or outcome != "wrong":
            details.append((name, stage, outcome, "na", "na"))
            continue
        bpm = observed["reported_bpm"]
        if (not isinstance(reference, (int, float)) or
                not math.isfinite(reference) or reference <= 0 or
                not isinstance(bpm, (int, float)) or not math.isfinite(bpm) or
                observed["held"] or abs(bpm - reference) <= .20 + 1e-9):
            raise ValueError("source-bit wrong-stage contradiction")
        rows = []
        for row, mask in zip(board, bits):
            try:
                value = float(row["bpm"])
            except (KeyError, ValueError, OverflowError, TypeError) as exc:
                raise ValueError("source-bit BPM malformed") from exc
            if not math.isfinite(value) or value <= 0:
                raise ValueError("source-bit BPM nonfinite")
            rows.append((value, mask))
        precise = [mask for value, mask in rows
                   if abs(value - reference) <= .20 + 1e-9]
        output = [mask for value, mask in rows
                  if abs(value - bpm) <= .20 + 1e-9]
        stat = counts[stage]
        stat["wrong"] += 1
        stat["precise" if precise else "precise_missing"] += 1
        stat["output" if output else "output_missing"] += 1
        if precise:
            stat["precise_full"] += int(any(mask & FULL for mask in precise))
            stat["precise_primary"] += int(any(mask & PRIMARY for mask in precise))
            stat["precise_supplement_only"] += int(
                not any(mask & (FULL | PRIMARY) for mask in precise))
        if output:
            stat["output_full"] += int(any(mask & FULL for mask in output))
            stat["output_primary"] += int(any(mask & PRIMARY for mask in output))
            stat["output_supplement_only"] += int(
                not any(mask & (FULL | PRIMARY) for mask in output))
        def merged(m):
            mask = 0
            for item in m:
                mask |= item
            return str(mask)
        details.append((name, stage, outcome, merged(precise), merged(output)))
    for stage, stat in counts.items():
        baseline = stages["stages"][stage]
        if (stat["wrong"] != baseline["clear_wrong"] or
                stat["wrong"] != stat["precise"] + stat["precise_missing"] or
                stat["wrong"] != stat["output"] + stat["output_missing"] or
                stat["precise_supplement_only"] > stat["precise"] or
                stat["output_supplement_only"] > stat["output"]):
            raise ValueError("source-bit stage denominator mismatch")
    totals = {key: sum(x[key] for x in counts.values()) for key in COUNTS}
    if (sum(x["clear"] for x in stages["stages"].values()) != 11 or
            sum(x["risk"] for x in stages["stages"].values()) != 5):
        raise ValueError("source-bit 11+5 denominator mismatch")
    if cohorts["clear"]["cases"] != 11 or cohorts["risk"]["cases"] != 5:
        raise ValueError("source-bit cohort case denominator mismatch")
    for stat in cohorts.values():
        if (stat["cases_with_board"] > stat["cases"] or
                stat["candidate_rows"] > stat["cases"] * 32 or
                stat["candidate_full_rows"] > stat["candidate_rows"] or
                stat["candidate_primary_rows"] > stat["candidate_rows"] or
                stat["candidate_supplement_only_rows"] > stat["candidate_rows"] or
                any(stat[key] > stat["cases"] for key in (
                    "cases_with_full", "cases_with_primary",
                    "cases_with_supplement_only"))):
            raise ValueError("source-bit cohort candidate denominator mismatch")
    return {"stages": counts, "totals": totals, "cohorts": cohorts,
            "details": details,
            "interpretation": "merged_review_sources_not_primary_eligibility"}
