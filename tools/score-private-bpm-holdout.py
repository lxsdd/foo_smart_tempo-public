#!/usr/bin/env python3
"""Private, offline-only Smart Tempo BPM holdout scoring.

Generate a local reference template from *complete* foobar verbose telemetry,
then fill independently verified reference BPM values and score offline.
No audio/tag writes, network, track-level output, or public CI owner inputs.
This script never selects a BPM; references are read only after analysis.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import re
import runpy
import sys

REFERENCE_SCHEMA = "smart_tempo_private_reference_v1"
CLASSES = frozenset({"clear", "ambiguous", "no_tempo"})
FIELDS = frozenset({"track_key", "subsong", "label", "reference_class", "reference_bpm"})
TOLERANCE_EXACT = 0.05
TOLERANCE_NEAR = 0.20


class HoldoutError(ValueError):
    """Privacy-safe error; never carry private labels or raw paths."""


def parse_current_verbose_log(path: Path):
    # Reuse the already-tested MW1 strict completeness/provenance parser.
    tool_path = Path(__file__).resolve().with_name("compare-mir-window-runs.py")
    parser = runpy.run_path(str(tool_path))["parse_log"]
    return parser(path)


def assert_private_destination(path: Path):
    resolved = path.resolve()
    # Reference templates contain track labels and hashed track identities.
    # Keep them out of *any* Git working tree, not only this repository.
    for ancestor in (resolved.parent, *resolved.parents):
        if (ancestor / ".git").exists():
            raise HoldoutError("reference destination inside Git worktree")
    if resolved.exists() or resolved.parent == resolved:
        raise HoldoutError("reference destination exists or is invalid")
    if not resolved.parent.is_dir():
        raise HoldoutError("reference destination parent missing")


def make_template(tracks: dict) -> dict:
    if not tracks:
        raise HoldoutError("no complete measured tracks")
    cases = []
    for key, track in sorted(
        tracks.items(), key=lambda item: (item[1].label.casefold(), item[0])
    ):
        cases.append({
            "track_key": str(key[0]),
            "subsong": key[1],
            "label": track.label,
            "reference_class": "clear",
            "reference_bpm": None,
        })
    return {"schema": REFERENCE_SCHEMA, "cases": cases}


def read_references(path: Path, tracks: dict) -> dict:
    raw = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(raw, dict) or set(raw) != {"schema", "cases"}:
        raise HoldoutError("invalid reference container")
    if raw["schema"] != REFERENCE_SCHEMA or not isinstance(raw["cases"], list):
        raise HoldoutError("unsupported reference schema")
    keyed = {}
    for row in raw["cases"]:
        if not isinstance(row, dict) or set(row) != FIELDS:
            raise HoldoutError("invalid reference row fields")
        key = (row["track_key"], row["subsong"])
        if (not isinstance(key[0], str) or
                not re.fullmatch(r"[0-9]+", key[0]) or
                type(key[1]) is not int or key[1] < 0 or
                key not in tracks or key in keyed or
                row["label"] != tracks[key].label):
            raise HoldoutError("reference identity, label or cardinality mismatch")
        kind = row["reference_class"]
        if kind not in CLASSES:
            raise HoldoutError("unknown reference class")
        bpm = row["reference_bpm"]
        if kind == "clear":
            if (type(bpm) not in (int, float) or
                    not math.isfinite(bpm) or not 1.0 <= bpm <= 1000.0):
                raise HoldoutError("clear reference requires finite BPM")
        elif bpm is not None:
            raise HoldoutError("ambiguous/no-tempo references must have null BPM")
        keyed[key] = {"class": kind, "bpm": bpm}
    if set(keyed) != set(tracks):
        raise HoldoutError("partial or extra reference identities")
    return keyed


def top_local_exact_bpm(track):
    # Shadow-only descriptive evidence. Do NOT promote to production output.
    # The MW1 parser emits candidate tuples in CANDIDATE_FIELDS order:
    # index 2 local_exact_bpm; index 3 local_exact_score.
    valid = []
    for candidate in track.candidates:
        if len(candidate) < 4:
            raise HoldoutError("malformed candidate evidence")
        try:
            bpm, score = float(candidate[2]), float(candidate[3])
        except (TypeError, ValueError) as exc:
            raise HoldoutError("non-numeric candidate evidence") from exc
        if not (math.isfinite(bpm) and math.isfinite(score)):
            raise HoldoutError("non-finite candidate evidence")
        if bpm > 0:
            valid.append((score, bpm))
    return max(valid)[1] if valid else None


def evaluate_holdout(tracks: dict, references: dict) -> dict:
    if not tracks or set(tracks) != set(references):
        raise HoldoutError("incomplete private audit population")
    counts = dict(total=len(tracks), clear_n=0, clear_accepted_n=0,
                  clear_within_0_05_n=0, clear_within_0_20_n=0,
                  clear_wrong_accepted_n=0, clear_held_n=0,
                  ambiguous_n=0, ambiguous_accepted_n=0,
                  no_tempo_n=0, no_tempo_false_accept_n=0,
                  shadow_clear_within_0_20_n=0,
                  shadow_ambiguous_proposals_n=0,
                  shadow_no_tempo_proposals_n=0)
    for key, track in tracks.items():
        expected = references[key]
        kind = expected["class"]
        final = track.final
        accepted = final["state"] == "WRITE"
        if final["state"] not in ("WRITE", "HOLD"):
            raise HoldoutError("unknown terminal output")
        observed = final["bpm"]
        if accepted:
            if (type(observed) not in (int, float) or
                    not math.isfinite(observed) or observed <= 0.0):
                raise HoldoutError("invalid accepted BPM")
        elif observed is not None:
            raise HoldoutError("hold must not contain accepted BPM")
        shadow = top_local_exact_bpm(track)
        if kind == "clear":
            counts["clear_n"] += 1
            target = expected["bpm"]
            if accepted:
                counts["clear_accepted_n"] += 1
                error = abs(observed - target)
                counts["clear_within_0_05_n"] += error <= TOLERANCE_EXACT + 1e-9
                counts["clear_within_0_20_n"] += error <= TOLERANCE_NEAR + 1e-9
                counts["clear_wrong_accepted_n"] += error > TOLERANCE_NEAR + 1e-9
            else:
                counts["clear_held_n"] += 1
            if shadow is not None:
                counts["shadow_clear_within_0_20_n"] += (
                    abs(shadow - target) <= TOLERANCE_NEAR + 1e-9)
        elif kind == "ambiguous":
            counts["ambiguous_n"] += 1
            counts["ambiguous_accepted_n"] += bool(accepted)
            counts["shadow_ambiguous_proposals_n"] += shadow is not None
        else:
            counts["no_tempo_n"] += 1
            counts["no_tempo_false_accept_n"] += bool(accepted)
            counts["shadow_no_tempo_proposals_n"] += shadow is not None
    counts["unsafe_accepted_n"] = (
        counts["clear_wrong_accepted_n"] +
        counts["ambiguous_accepted_n"] +
        counts["no_tempo_false_accept_n"])
    counts["quality_verdict"] = (
        "NO_GO" if counts["unsafe_accepted_n"] else
        "REVIEW_HOLDS" if counts["clear_held_n"] else
        "PRIVATE_HOLDOUT_ONLY_PASS" if
        counts["clear_within_0_05_n"] == counts["clear_n"] else
        "REVIEW_PRECISION")
    return counts


def main() -> int:
    p = argparse.ArgumentParser(description="Private offline aggregate BPM audit")
    p.add_argument("--log", required=True, type=Path,
                   help="private local foobar verbose log")
    group = p.add_mutually_exclusive_group(required=True)
    group.add_argument("--prepare", type=Path,
                       help="new local JSON template, never inside Git tree")
    group.add_argument("--references", type=Path,
                       help="filled local JSON reference template")
    args = p.parse_args()
    try:
        if args.prepare is not None:
            assert_private_destination(args.prepare)
        measured = parse_current_verbose_log(args.log)
        if args.prepare is not None:
            template = make_template(measured)
            with args.prepare.open("x", encoding="utf-8") as out:
                json.dump(template, out, indent=2, ensure_ascii=False)
                out.write("\n")
            print("PRIVATE_BPM_REFERENCE_TEMPLATE=CREATED_LOCALLY")
            print(f"PRIVATE_BPM_TEMPLATE_CASES={len(measured)}")
            return 0
        refs = read_references(args.references, measured)
        report = evaluate_holdout(measured, refs)
        for key, value in report.items():
            print(f"PRIVATE_BPM_{key.upper()}={value}")
        return 0
    except Exception:
        # No raw exception, path, track label or per-track BPM may leak into
        # the console or a captured CI log. Source fixtures in public CI only.
        print("PRIVATE_BPM_AUDIT=FAIL", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
