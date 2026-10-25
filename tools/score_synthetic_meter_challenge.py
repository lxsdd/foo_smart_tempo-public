#!/usr/bin/env python3
"""Post-host synthetic metrical BPM scorer; no candidate selection or writes."""
from __future__ import annotations
import argparse
import csv
import json
import math
from pathlib import Path
from generate_synthetic_meter_challenge import CASES


def score(manifest: dict, observed: list[dict]) -> dict:
    if manifest.get("schema") != "synthetic_meter_challenge_v2":
        raise ValueError("unexpected meter schema")
    rows = manifest.get("cases")
    if not isinstance(rows, list) or len(rows) != 5 or len(observed) != 5:
        raise ValueError("five-case meter inventory mismatch")
    stats = dict(clear=0, correct_005=0, correct_020=0, wrong=0,
                 held=0, beat_unit_confusion=0, ambiguous=0,
                 ambiguous_held=0, ambiguous_accepted=0)
    detail = []
    for spec, row, outcome in zip(CASES, rows, observed):
        name, bpm, meter, unit, _ = spec
        if (row.get("id") != name or row.get("reference_bpm") != bpm or
                row.get("meter") != meter or row.get("beat_unit") != unit or
                row.get("corpus_partition") != "separate_synthetic_meter_challenge_v2" or
                row.get("ambiguity") != ("meter_underdetermined" if bpm is None else "none") or
                outcome.get("id") != name):
            raise ValueError("meter oracle/observation identity drift")
        try:
            got = float(outcome["bpm"])
        except (KeyError, ValueError, TypeError, OverflowError) as exc:
            raise ValueError("invalid observed BPM") from exc
        held = outcome.get("held")
        if (not math.isfinite(got) or got < 0 or
                type(held) is not bool or (held and got != 0) or
                (not held and got == 0)):
            raise ValueError("contradictory or non-finite host BPM")
        if bpm is None:
            stats["ambiguous"] += 1
            stats["ambiguous_held" if held else "ambiguous_accepted"] += 1
            detail.append((name, "ambiguous_hold" if held else "unsafe_ambiguous_accept"))
        else:
            stats["clear"] += 1
            if held:
                stats["held"] += 1
                label = "held"
            elif abs(got - bpm) <= .05 + 1e-9:
                stats["correct_005"] += 1
                stats["correct_020"] += 1
                label = "correct_005"
            elif abs(got - bpm) <= .20 + 1e-9:
                stats["correct_020"] += 1
                label = "correct_020"
            else:
                stats["wrong"] += 1
                # 3/4 quarters versus 6/8 dotted quarters, not half/double tempo.
                ratio = got / bpm
                is_confusion = min(abs(ratio - 1.5), abs(ratio - (2 / 3))) <= .015
                stats["beat_unit_confusion"] += int(is_confusion)
                label = "wrong_3_to_2" if is_confusion else "wrong"
            detail.append((name, label))
    if (stats["clear"] != 4 or stats["ambiguous"] != 1 or
            stats["correct_020"] + stats["wrong"] + stats["held"] != 4 or
            stats["ambiguous_held"] + stats["ambiguous_accepted"] != 1):
        raise ValueError("meter denominator inconsistency")
    return {"counts": stats, "details": detail,
            "verdict": "NO_GO" if stats["wrong"] or stats["ambiguous_accepted"] else "RESEARCH_ONLY_NOT_MUSICAL_GO"}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--observed", required=True, type=Path)
    args = parser.parse_args()
    ref = json.loads(args.manifest.read_text(encoding="utf-8-sig"))
    with args.observed.open(newline="", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        if tuple(reader.fieldnames or ()) != ("id", "bpm", "held"):
            raise ValueError("unexpected public meter observation columns")
        obs = []
        for row in reader:
            if set(row) != {"id", "bpm", "held"} or row["held"] not in ("0", "1"):
                raise ValueError("invalid meter row")
            obs.append(dict(id=row["id"], bpm=row["bpm"], held=row["held"] == "1"))
    result = score(ref, obs)
    for key, value in result["counts"].items():
        print(f"METER_CHALLENGE_{key.upper()}={value}")
    print(f"METER_CHALLENGE_VERDICT={result['verdict']}")


if __name__ == "__main__":
    main()
