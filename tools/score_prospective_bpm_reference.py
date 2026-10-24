#!/usr/bin/env python3
"""Post-analysis, synthetic-only prospective BPM and abstention comparison.

The profile below was frozen after exploring the earlier 80-case corpus, before
these 20 parameter-holdout WAVs were measured in the real foobar host. This is
not an independent recording domain or owner-music release qualification.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path

from generate_synthetic_bpm_reference import PROSPECTIVE_CASES
from score_synthetic_bpm_reference import (
    evaluate, load_observed, shadow_candidate_choice,
)

CANDIDATE_FIELDS = (
    "id", "rank", "bpm", "evidence", "support", "winner", "local_exact",
)
PROFILE_MIN_LOCAL = 0.95
PROFILE_HARMONIC_MARGIN = 0.10


def load_manifest(path: Path) -> dict:
    with path.open("r", encoding="utf-8-sig") as f:
        manifest = json.load(f)
    expected = PROSPECTIVE_CASES
    rows = manifest.get("cases", ())
    if manifest.get("schema") != "synthetic_bpm_reference_v1" or (
            not isinstance(rows, list) or len(rows) != len(expected)):
        raise ValueError("Invalid prospective synthetic reference schema")
    if len({row.get("id") for row in rows}) != len(expected):
        raise ValueError("Duplicate prospective fixture ID")
    for row, (name, bpm, style) in zip(rows, expected):
        ambiguity = (
            "meter_underdetermined" if style in ("half_pulse", "sparse") else
            "tempo_nonstationary" if style.startswith("tempo_switch") else
            "none"
        )
        truth = row.get("reference_bpm")
        if (row.get("id") != name or row.get("style") != style or
                row.get("ambiguity") != ambiguity or
                (truth is None) != (bpm is None) or
                (bpm is not None and (not isinstance(truth, (int, float)) or
                                     not math.isfinite(truth) or
                                     abs(truth - bpm) > 1e-9))):
            raise ValueError("Prospective reference identity mismatch")
    return manifest


def load_candidates(path: Path) -> dict[str, list[dict]]:
    known = {name for name, _, _ in PROSPECTIVE_CASES}
    grouped: dict[str, list[dict]] = {name: [] for name in known}
    with path.open("r", encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        if tuple(reader.fieldnames or ()) != CANDIDATE_FIELDS:
            raise ValueError("Invalid prospective candidate schema")
        for item in reader:
            if set(item) != set(CANDIDATE_FIELDS) or item["id"] not in known:
                raise ValueError("Unrecognized public prospective candidate")
            rank = int(item["rank"])
            values = [float(item[k]) for k in
                      ("bpm", "evidence", "support", "winner", "local_exact")]
            if (not 1 <= rank <= 256 or
                    len(grouped[item["id"]]) >= 32 or
                    not all(map(math.isfinite, values)) or
                    values[0] <= 0 or values[2] < 0 or values[3] < 0):
                raise ValueError("Invalid or unbounded prospective candidate")
            grouped[item["id"]].append(item)
    return grouped


def aggregate(rows: list[dict], candidates: dict[str, list[dict]]) -> tuple[dict, dict]:
    """Choose without labels, and apply labels only in the separate summary."""
    proposals = {
        row["id"]: shadow_candidate_choice(
            candidates[row["id"]], PROFILE_MIN_LOCAL, PROFILE_HARMONIC_MARGIN
        )
        for row in rows
    }
    reference = dict(clear=0, accurate=0, wrong=0, held=0,
                     ambiguous_accepted=0, no_tempo_accepted=0)
    shadow = reference.copy()
    for row in rows:
        reported = None if row["held"] else row["reported_bpm"]
        for out, selected in ((reference, reported),
                              (shadow, proposals[row["id"]])):
            accepted = selected is not None and selected > 0
            if row["ambiguous"]:
                out["ambiguous_accepted"] += bool(accepted)
            elif row["reference_bpm"] is None:
                out["no_tempo_accepted"] += bool(accepted)
            else:
                out["clear"] += 1
                if not accepted:
                    out["held"] += 1
                elif abs(selected - row["reference_bpm"]) <= .20 + 1e-9:
                    out["accurate"] += 1
                else:
                    out["wrong"] += 1
    return reference, shadow


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--observed", type=Path, required=True)
    parser.add_argument("--candidates", type=Path, required=True)
    args = parser.parse_args()
    try:
        manifest = load_manifest(args.manifest)
        observed = load_observed(args.observed)
        if len(observed) != len(PROSPECTIVE_CASES):
            raise ValueError("Incomplete prospective real-host observation set")
        grouped = load_candidates(args.candidates)
        scored = evaluate(manifest, observed)
        production, shadow = aggregate(scored["results"], grouped)
    except (OSError, ValueError, KeyError, TypeError, OverflowError) as exc:
        parser.error(str(exc))
    print(f"PROSPECTIVE_REAL_HOST_CASES={len(PROSPECTIVE_CASES)}")
    print("PROSPECTIVE_PROFILE_FROZEN=min_local_0.95,harmonic_margin_0.10")
    for label, counts in (("PRODUCTION", production), ("SHADOW", shadow)):
        for key, value in counts.items():
            print(f"PROSPECTIVE_{label}_{key.upper()}={value}")
    unsafe = (shadow["wrong"] + shadow["ambiguous_accepted"] +
              shadow["no_tempo_accepted"])
    print("PROSPECTIVE_SHADOW_QUALITY_VERDICT=" +
          ("NO_GO" if unsafe else
           "REVIEW_HOLDS" if shadow["held"] else "SYNTHETIC_ONLY_PASS"))
    print("PROSPECTIVE_SHADOW_NEVER_APPLIED_TO_AUDIO_OR_TAGS=TRUE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
