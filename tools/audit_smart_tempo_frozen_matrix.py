#!/usr/bin/env python3
"""Post-host, public-synthetic-only paired evidence scorecard.

Research-only: neither predicts nor changes the production BPM. The native
measurements MUST be completed independently before a reference manifest is
provided to this evaluator. All per-case/architecture records must be complete,
source-SHA-exact, source-blind and explicit about holds and write protection.
"""
from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path

SHA = re.compile(r"[0-9a-f]{40}\Z")
CASE = re.compile(r"(?:pulse|probe|challenge|window|temporal|pass|meter|sweep|silence|synthetic)[A-Za-z0-9_.:-]{0,96}\Z")
CATEGORIES = frozenset(("clear", "meter_ambiguous", "nonstationary", "nonrhythmic"))
ARCHS = ("Win32", "x64")
MAX_CASES = 4096
MAX_JSON_BYTES = 4 * 1024 * 1024


class EvidenceError(ValueError):
    pass


def required_exact(data, keys, description):
    if type(data) is not dict or set(data) != set(keys):
        raise EvidenceError("invalid " + description + " fields")


def decimal(value, description, *, maximum=1e12):
    if type(value) not in (int, float) or not math.isfinite(value) or not 0 <= value <= maximum:
        raise EvidenceError("invalid " + description)
    return float(value)


def parse_json(path: Path):
    if not path.is_file() or not 0 < path.stat().st_size <= MAX_JSON_BYTES:
        raise EvidenceError("missing/oversized evidence file")

    def unique(pairs):
        output = {}
        for k, v in pairs:
            if k in output:
                raise EvidenceError("duplicate JSON field")
            output[k] = v
        return output

    try:
        return json.loads(path.read_text(encoding="utf-8-sig"),
                          object_pairs_hook=unique,
                          parse_constant=lambda _v: (_ for _ in ()).throw(EvidenceError("nonfinite JSON")))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise EvidenceError("invalid JSON") from exc


def check_manifest(manifest):
    required_exact(manifest, ("schema", "cohort", "cases"), "reference manifest")
    if (manifest["schema"] != "smart_tempo_frozen_matrix_reference_v1" or manifest["cohort"] != "public_synthetic_frozen"):
        raise EvidenceError("manifest must contain frozen public synthetic evidence")
    cases = manifest["cases"]
    if type(cases) is not list or not 1 <= len(cases) <= MAX_CASES:
        raise EvidenceError("invalid frozen matrix case count")
    expected = {}
    for row in cases:
        required_exact(row, ("id", "reference_bpm", "class"), "reference case")
        key = row["id"]
        if type(key) is not str or not CASE.fullmatch(key) or key in expected:
            raise EvidenceError("duplicate or nonpublic synthetic case ID")
        classification = row["class"]
        bpm = row["reference_bpm"]
        if type(classification) is not str or classification not in CATEGORIES:
            raise EvidenceError("unknown reference class")
        if classification == "clear":
            if type(bpm) not in (int, float) or not math.isfinite(bpm) or not 0 < bpm <= 400:
                raise EvidenceError("clear case missing valid reference BPM")
        elif bpm is not None and (
                type(bpm) not in (int, float) or not math.isfinite(bpm) or not 0 < bpm <= 400):
            # Ambiguous cases may have a declared beat grid; a grid is not
            # permission to label one metrical interpretation correct.
            raise EvidenceError("invalid optional ambiguous reference BPM")
        if classification in ("nonstationary", "nonrhythmic") and bpm is not None:
            raise EvidenceError("negative case fabricated a single BPM oracle")
        expected[key] = row
    return expected


def validate_host(report, expected, sha, arch):
    required_exact(report, ("schema", "architecture", "source_sha", "cases"), "host report")
    if (report["schema"] != "smart_tempo_frozen_matrix_host_v1" or
            report["architecture"] != arch or report["source_sha"] != sha):
        raise EvidenceError("stale source SHA or incompatible host report")
    cases = report["cases"]
    if type(cases) is not list or len(cases) != len(expected):
        raise EvidenceError("incomplete native host case coverage")
    out = {}
    for row in cases:
        required_exact(row, ("id", "native_bpm", "native_held", "shadow_bpm",
                             "shadow_held", "cpu_ms", "wall_ms", "peak_rss_bytes",
                             "native_source_indexed", "supplemental_witnessed", "shadow_source_indexed", "media_no_write"), "host case")
        key = row["id"]
        if type(key) is not str or key not in expected or key in out:
            raise EvidenceError("missing/duplicate/untrusted native host case")
        for name in ("native_bpm", "shadow_bpm"):
            decimal(row[name], name, maximum=2000)
        for name in ("cpu_ms", "wall_ms"):
            decimal(row[name], name)
        if type(row["peak_rss_bytes"]) is not int or not 0 < row["peak_rss_bytes"] <= 1e12:
            raise EvidenceError("invalid native peak RSS witness")
        for name in ("native_held", "shadow_held", "native_source_indexed", "supplemental_witnessed", "shadow_source_indexed", "media_no_write"):
            if type(row[name]) is not bool:
                raise EvidenceError("untrusted nonboolean " + name)
        if not row["media_no_write"]:
            raise EvidenceError("native media mutation gate not satisfied")
        if row["native_bpm"] > 0 and not row["native_held"] and not (row["native_source_indexed"] or row["supplemental_witnessed"]):
            raise EvidenceError("native final BPM lacks indexed or independently witnessed supplemental source")
        # A shadow proposal must come from the existing measured candidate
        # board, never from a post-hoc reference BPM. This boolean is a
        # prerequisite only; genuine indexed-source lineage still requires
        # independently audited native host traces.
        if row["shadow_bpm"] > 0 and not row["shadow_held"] and not row["shadow_source_indexed"]:
            raise EvidenceError("shadow BPM lacks indexed measured candidate source")
        if row["wall_ms"] + .001 < row["cpu_ms"] / 128:
            raise EvidenceError("impossible CPU/wall clock resource witness")
        out[key] = row
    if set(out) != set(expected):
        raise EvidenceError("native host case identity drift")
    return out


def outcome(row, kind, ref):
    value = row[kind + "_bpm"]
    held = row[kind + "_held"]
    accepted = not held and value > 0
    category = ref["class"]
    if category != "clear":
        return "held" if not accepted else "risk_accept"
    if not accepted:
        return "held"
    error = abs(value - ref["reference_bpm"])
    if error <= .05 + 1e-9:
        return "correct_005"
    if error <= .20 + 1e-9:
        return "correct_020"
    ratio = value / ref["reference_bpm"]
    if min(abs(ratio - 2), abs(ratio - .5), abs(ratio - 1.5), abs(ratio - (2 / 3))) <= .02:
        return "metric_alias"
    return "wrong"


def percentile(values, p):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(0, math.ceil(p * len(ordered)) - 1)]


def score(manifest, host_reports, source_sha):
    if type(source_sha) is not str or not SHA.fullmatch(source_sha):
        raise EvidenceError("invalid expected source SHA")
    expected = check_manifest(manifest)
    if type(host_reports) is not list or len(host_reports) != 2:
        raise EvidenceError("both original Windows architectures required")
    if (type(host_reports[0]) is not dict or type(host_reports[1]) is not dict or
            any(type(r.get("architecture")) is not str for r in host_reports) or
            {r.get("architecture") for r in host_reports} != set(ARCHS)):
        raise EvidenceError("missing or duplicate original host architecture")
    rows = {arch: validate_host(next(r for r in host_reports if r["architecture"] == arch),
                                expected, source_sha, arch) for arch in ARCHS}
    metrics = {}
    case_ids = tuple(expected)
    for arch in ARCHS:
        data = rows[arch]
        counts = {key: {k: 0 for k in ("held", "risk_accept", "correct_005",
                                      "correct_020", "metric_alias", "wrong")}
                  for key in ("native", "shadow")}
        changes = {k: 0 for k in ("gain_020", "loss_020", "new_risk", "avoided_risk",
                                  "held_became_accept", "accept_became_held")}
        for case_id in case_ids:
            native = outcome(data[case_id], "native", expected[case_id])
            shadow = outcome(data[case_id], "shadow", expected[case_id])
            counts["native"][native] += 1
            counts["shadow"][shadow] += 1
            changes["gain_020"] += int(shadow in ("correct_005", "correct_020") and native not in ("correct_005", "correct_020"))
            changes["loss_020"] += int(native in ("correct_005", "correct_020") and shadow not in ("correct_005", "correct_020"))
            changes["new_risk"] += int(shadow == "risk_accept" and native != "risk_accept")
            changes["avoided_risk"] += int(native == "risk_accept" and shadow != "risk_accept")
            changes["held_became_accept"] += int(native == "held" and shadow != "held")
            changes["accept_became_held"] += int(shadow == "held" and native != "held")
        metrics[arch] = {"count": len(case_ids), "outcomes": counts,
                         "cumulative_precision": {
                            model: {"within_005": counts[model]["correct_005"],
                                    "within_020": counts[model]["correct_005"] + counts[model]["correct_020"],
                                    "auto_wrong_or_alias": counts[model]["wrong"] + counts[model]["metric_alias"],
                                    "risk_accept": counts[model]["risk_accept"],
                                    "held": counts[model]["held"]}
                            for model in ("native", "shadow")},
                         "paired_changes": changes,
                         "resources": {
                             "cpu_ms_p95": percentile([data[c]["cpu_ms"] for c in case_ids], .95),
                             "cpu_ms_p99": percentile([data[c]["cpu_ms"] for c in case_ids], .99),
                             "wall_ms_p95": percentile([data[c]["wall_ms"] for c in case_ids], .95),
                             "wall_ms_p99": percentile([data[c]["wall_ms"] for c in case_ids], .99),
                             "peak_rss_bytes_max": max(data[c]["peak_rss_bytes"] for c in case_ids)}}
    disagreements = []
    for case_id in case_ids:
        a, b = rows["Win32"][case_id], rows["x64"][case_id]
        if (outcome(a, "native", expected[case_id]) != outcome(b, "native", expected[case_id]) or
                outcome(a, "shadow", expected[case_id]) != outcome(b, "shadow", expected[case_id]) or
                a["native_held"] != b["native_held"] or a["shadow_held"] != b["shadow_held"] or
                abs(a["native_bpm"] - b["native_bpm"]) > .05 or
                abs(a["shadow_bpm"] - b["shadow_bpm"]) > .05):
            disagreements.append(case_id)
    if disagreements:
        raise EvidenceError("cross-architecture per-case BPM/hold drift")
    clear = sum(row["class"] == "clear" for row in expected.values())
    return {"schema": "smart_tempo_frozen_matrix_score_v1", "source_sha": source_sha,
            "synthetic_case_configurations": len(expected), "clear": clear,
            "ambiguous_or_negative": len(expected) - clear,
            "architectures": metrics, "cross_arch_case_parity": True,
            "musical_quality_admission": "NOT_ESTABLISHED",
            "production_selector_admission": "NO_GO",
            "private_blind_holdout": "NOT_RUN"}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--manifest", required=True, type=Path)
    p.add_argument("--report", required=True, type=Path, action="append")
    p.add_argument("--expected-source-sha", required=True)
    p.add_argument("--json-output", type=Path)
    args = p.parse_args()
    try:
        if len(args.report) != 2:
            raise EvidenceError("two architecture reports required")
        result = score(parse_json(args.manifest),
                       [parse_json(path) for path in args.report], args.expected_source_sha)
        if args.json_output:
            if args.json_output.exists():
                raise EvidenceError("refuse overwrite existing research report")
            args.json_output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                                        encoding="utf-8")
    except EvidenceError as exc:
        p.error(str(exc))
    print("SMART_TEMPO_FROZEN_MATRIX_SOURCES=EXACT_SHA")
    print("SMART_TEMPO_FROZEN_MATRIX_ARCHITECTURES=Win32,x64")
    print("SMART_TEMPO_FROZEN_MATRIX_CASES=" + str(result["synthetic_case_configurations"]))
    print("SMART_TEMPO_FROZEN_MATRIX_PRODUCT_ADMISSION=NO_GO")


if __name__ == "__main__":
    main()
