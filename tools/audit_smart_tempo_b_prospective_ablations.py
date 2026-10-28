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


class ProspectiveError(ValueError):
    pass


def validate(reports, expected_source_sha=None, require_both=True):
    if type(reports) is not list or len(reports) != (2 if require_both else 1):
        raise ProspectiveError("missing host architecture")
    if expected_source_sha is not None and not re.fullmatch(r"[a-f0-9]{40}", expected_source_sha):
        raise ProspectiveError("invalid expected source SHA")
    seen, shas, totals = set(), set(), {}
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
        for i, row in enumerate(cases):
            if type(row) is not dict or set(row) != {"case", "ablation", "media_no_write"}:
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
        totals[arch] = aggregate
    if len(shas) != 1:
        raise ProspectiveError("mixed product source SHAs")
    return {"source_sha": next(iter(shas)), "architectures": totals, "n_per_architecture": len(CASES)}


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
    for arch in ("Win32", "x64"):
        if arch not in result["architectures"]:
            continue
        for model in MODELS:
            for field in FIELDS:
                print("SMART_TEMPO_PROSPECTIVE_" + arch.upper() + "_" + model + "_" + field +
                      "=" + str(result["architectures"][arch][model][field]))


if __name__ == "__main__":
    main()
